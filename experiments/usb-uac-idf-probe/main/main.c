#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "usb/usb_host.h"

static const char *TAG = "probe";
static QueueHandle_t s_new_devices;

#define FIFO_RX_LINES   72
#define FIFO_NPTX_LINES 32
#define FIFO_PTX_LINES  96
#define FIFO_TOTAL      (FIFO_RX_LINES + FIFO_NPTX_LINES + FIFO_PTX_LINES)
#define IN_MPS_LIMIT    ((FIFO_RX_LINES - 2) * 4)
#define NPTX_MPS_LIMIT  (FIFO_NPTX_LINES * 4)
#define PTX_MPS_LIMIT   (FIFO_PTX_LINES * 4)

_Static_assert(FIFO_TOTAL == 200, "ESP32-S3 USB host FIFO budget must be 200 lines");

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static const char *speed_name(usb_speed_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:  return "low-speed";
    case USB_SPEED_FULL: return "full-speed (12Mbps)";
    case USB_SPEED_HIGH: return "high-speed";
    default:             return "unknown";
    }
}

static const char *xfer_name(uint8_t attributes)
{
    switch (attributes & 0x03) {
    case 0: return "control";
    case 1: return "isochronous";
    case 2: return "bulk";
    case 3: return "interrupt";
    default: return "unknown";
    }
}

static void print_usb_string(const char *label, const usb_str_desc_t *desc)
{
    char out[128] = {0};
    if (desc == NULL || desc->bLength < 2) {
        ESP_LOGI(TAG, "%s=<unavailable>", label);
        return;
    }

    size_t chars = (desc->bLength - 2) / 2;
    if (chars > sizeof(out) - 1) {
        chars = sizeof(out) - 1;
    }
    for (size_t i = 0; i < chars; ++i) {
        uint16_t c = desc->wData[i];
        out[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    ESP_LOGI(TAG, "%s=%s", label, out);
}

static void print_raw_descriptor(const uint8_t *d, size_t len)
{
    printf("[probe]   raw=");
    for (size_t i = 0; i < len; ++i) {
        printf("%02x%s", d[i], (i + 1 == len) ? "" : " ");
    }
    printf("\n");
}

static void dump_configuration(const usb_config_desc_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "configuration descriptor unavailable");
        return;
    }

    const uint8_t *raw = (const uint8_t *)cfg;
    const size_t total = cfg->wTotalLength;
    ESP_LOGI(TAG,
             "config value=%u total=%u interfaces=%u attributes=0x%02x max-power=%umA",
             cfg->bConfigurationValue,
             (unsigned)total,
             cfg->bNumInterfaces,
             cfg->bmAttributes,
             cfg->bMaxPower * 2U);

    uint8_t iface = 0;
    uint8_t alt = 0;
    uint8_t iface_class = 0;
    uint8_t iface_subclass = 0;
    uint8_t iface_protocol = 0;
    bool in_interface = false;

    for (size_t pos = 0; pos + 2 <= total;) {
        const uint8_t *d = raw + pos;
        const uint8_t len = d[0];
        const uint8_t type = d[1];

        if (len < 2 || pos + len > total) {
            ESP_LOGE(TAG, "malformed descriptor pos=%u len=%u total=%u",
                     (unsigned)pos, len, (unsigned)total);
            break;
        }

        switch (type) {
        case 0x02: // Configuration
            break;

        case 0x0b: // Interface Association Descriptor
            if (len >= 8) {
                ESP_LOGI(TAG,
                         "IAD first-if=%u count=%u class=0x%02x subclass=0x%02x protocol=0x%02x",
                         d[2], d[3], d[4], d[5], d[6]);
            }
            break;

        case 0x04: // Interface
            if (len >= 9) {
                iface = d[2];
                alt = d[3];
                iface_class = d[5];
                iface_subclass = d[6];
                iface_protocol = d[7];
                in_interface = true;
                ESP_LOGI(TAG,
                         "interface=%u alt=%u endpoints=%u class=0x%02x subclass=0x%02x protocol=0x%02x",
                         iface, alt, d[4], iface_class, iface_subclass, iface_protocol);
                if (iface_class == 0x01) {
                    const char *role = iface_subclass == 0x01 ? "AudioControl" :
                                       iface_subclass == 0x02 ? "AudioStreaming" :
                                       iface_subclass == 0x03 ? "MIDIStreaming" : "Audio-other";
                    ESP_LOGI(TAG, "audio interface=%u alt=%u role=%s", iface, alt, role);
                }
            }
            break;

        case 0x05: // Endpoint
            if (len >= 7) {
                const uint8_t ep = d[2];
                const uint8_t attr = d[3];
                const uint16_t mps_raw = le16(d + 4);
                const uint16_t mps = mps_raw & 0x07ff;
                const bool in = (ep & 0x80) != 0;
                ESP_LOGI(TAG,
                         "endpoint interface=%u alt=%u ep=0x%02x direction=%s transfer=%s attr=0x%02x MPS=%u raw-MPS=0x%04x interval=%u",
                         iface, alt, ep, in ? "IN" : "OUT", xfer_name(attr), attr,
                         mps, mps_raw, d[6]);

                if (in && mps > IN_MPS_LIMIT) {
                    ESP_LOGW(TAG, "endpoint 0x%02x requires IN MPS=%u > probe limit=%u",
                             ep, mps, IN_MPS_LIMIT);
                } else if (!in && (attr & 0x03) == 0x01 && mps > PTX_MPS_LIMIT) {
                    ESP_LOGW(TAG, "endpoint 0x%02x requires periodic OUT MPS=%u > probe limit=%u",
                             ep, mps, PTX_MPS_LIMIT);
                } else if (!in && (attr & 0x03) != 0x01 && mps > NPTX_MPS_LIMIT) {
                    ESP_LOGW(TAG, "endpoint 0x%02x requires non-periodic OUT MPS=%u > probe limit=%u",
                             ep, mps, NPTX_MPS_LIMIT);
                }
            }
            break;

        case 0x24: // Class-specific interface
            if (in_interface && len >= 3) {
                const uint8_t subtype = d[2];
                ESP_LOGI(TAG,
                         "CS-interface interface=%u alt=%u subclass=0x%02x protocol=0x%02x subtype=0x%02x len=%u",
                         iface, alt, iface_subclass, iface_protocol, subtype, len);

                // AudioControl HEADER. bcdADC is in bytes 3..4 for UAC1/UAC2.
                if (iface_class == 0x01 && iface_subclass == 0x01 && subtype == 0x01 && len >= 5) {
                    ESP_LOGI(TAG, "UAC bcdADC=0x%04x", le16(d + 3));
                }

                // UAC1 AudioStreaming AS_GENERAL.
                if (iface_class == 0x01 && iface_subclass == 0x02 && iface_protocol == 0x00 &&
                    subtype == 0x01 && len >= 7) {
                    ESP_LOGI(TAG, "UAC1 AS general terminal-link=%u delay=%u format-tag=0x%04x",
                             d[3], d[4], le16(d + 5));
                }

                // UAC1 FORMAT_TYPE I: channels/subframe/bits/sample rates.
                if (iface_class == 0x01 && iface_subclass == 0x02 && iface_protocol == 0x00 &&
                    subtype == 0x02 && len >= 8 && d[3] == 0x01) {
                    const uint8_t channels = d[4];
                    const uint8_t subframe = d[5];
                    const uint8_t bits = d[6];
                    const uint8_t freq_type = d[7];
                    ESP_LOGI(TAG,
                             "UAC1 Type-I channels=%u subframe=%u bytes bits=%u freq-type=%u",
                             channels, subframe, bits, freq_type);
                    if (freq_type == 0 && len >= 14) {
                        ESP_LOGI(TAG, "UAC1 rate-range=%" PRIu32 "..%" PRIu32 " Hz",
                                 le24(d + 8), le24(d + 11));
                    } else {
                        for (uint8_t i = 0; i < freq_type; ++i) {
                            size_t off = 8 + (size_t)i * 3;
                            if (off + 3 > len) {
                                ESP_LOGW(TAG, "sample-rate list truncated at index=%u", i);
                                break;
                            }
                            ESP_LOGI(TAG, "UAC1 rate[%u]=%" PRIu32 " Hz", i, le24(d + off));
                        }
                    }
                }
                print_raw_descriptor(d, len);
            }
            break;

        case 0x25: // Class-specific endpoint
            ESP_LOGI(TAG, "CS-endpoint interface=%u alt=%u len=%u", iface, alt, len);
            print_raw_descriptor(d, len);
            break;

        default:
            // Keep unknown descriptor types visible. Audio devices frequently
            // carry class-specific descriptors that are useful during bring-up.
            ESP_LOGI(TAG, "descriptor interface=%u alt=%u type=0x%02x len=%u",
                     iface, alt, type, len);
            if (type >= 0x20 || iface_class == 0x01) {
                print_raw_descriptor(d, len);
            }
            break;
        }

        pos += len;
    }
}

static void inspect_device(usb_host_client_handle_t client, uint8_t address)
{
    usb_device_handle_t dev = NULL;
    esp_err_t err = usb_host_device_open(client, address, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open address=%u failed: %s", address, esp_err_to_name(err));
        return;
    }

    usb_device_info_t info = {0};
    err = usb_host_device_info(dev, &info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "device address=%u speed=%s config=%u parent-port=%u",
                 address, speed_name(info.speed), info.bConfigurationValue, info.parent.port_num);
        print_usb_string("manufacturer", info.str_desc_manufacturer);
        print_usb_string("product", info.str_desc_product);
        print_usb_string("serial", info.str_desc_serial_num);
    } else {
        ESP_LOGE(TAG, "device-info address=%u failed: %s", address, esp_err_to_name(err));
    }

    const usb_device_desc_t *dev_desc = NULL;
    err = usb_host_get_device_descriptor(dev, &dev_desc);
    if (err == ESP_OK && dev_desc != NULL) {
        ESP_LOGI(TAG,
                 "device-desc vid=%04x pid=%04x bcdUSB=0x%04x bcdDevice=0x%04x class=0x%02x subclass=0x%02x protocol=0x%02x max-packet0=%u configs=%u",
                 dev_desc->idVendor, dev_desc->idProduct, dev_desc->bcdUSB, dev_desc->bcdDevice,
                 dev_desc->bDeviceClass, dev_desc->bDeviceSubClass, dev_desc->bDeviceProtocol,
                 dev_desc->bMaxPacketSize0, dev_desc->bNumConfigurations);
    } else {
        ESP_LOGE(TAG, "device-descriptor address=%u failed: %s", address, esp_err_to_name(err));
    }

    const usb_config_desc_t *cfg = NULL;
    err = usb_host_get_active_config_descriptor(dev, &cfg);
    if (err == ESP_OK && cfg != NULL) {
        dump_configuration(cfg);
    } else {
        ESP_LOGE(TAG, "config-descriptor address=%u failed: %s", address, esp_err_to_name(err));
    }

    err = usb_host_device_close(client, dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "close address=%u failed: %s", address, esp_err_to_name(err));
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event == NULL) {
        return;
    }

    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        uint8_t address = event->new_dev.address;
        ESP_LOGI(TAG, "NEW_DEV address=%u", address);
        if (xQueueSend(s_new_devices, &address, 0) != pdTRUE) {
            ESP_LOGW(TAG, "device queue full; dropped address=%u", address);
        }
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGI(TAG, "DEV_GONE handle=%p", event->dev_gone.dev_hdl);
    }
}

static void host_daemon_task(void *arg)
{
    TaskHandle_t owner = (TaskHandle_t)arg;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = NULL,
        .fifo_settings_custom = {
            .nptx_fifo_lines = FIFO_NPTX_LINES,
            .ptx_fifo_lines = FIFO_PTX_LINES,
            .rx_fifo_lines = FIFO_RX_LINES,
        },
        .peripheral_map = 0,
    };

    esp_err_t err = usb_host_install(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        xTaskNotifyGive(owner);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG,
             "USB host ready FIFO RX=%u NPTX=%u PTX=%u TOTAL=%u MPS-IN=%u bulk-OUT=%u periodic-OUT=%u",
             FIFO_RX_LINES, FIFO_NPTX_LINES, FIFO_PTX_LINES, FIFO_TOTAL,
             IN_MPS_LIMIT, NPTX_MPS_LIMIT, PTX_MPS_LIMIT);
    xTaskNotifyGive(owner);

    while (true) {
        uint32_t flags = 0;
        err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "host event loop failed: %s", esp_err_to_name(err));
            break;
        }
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "NEWO USB descriptor probe");
    ESP_LOGI(TAG, "ESP-IDF control-transfer-max=%u hubs=%s",
             CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE,
#ifdef CONFIG_USB_HOST_HUBS_SUPPORTED
             "enabled"
#else
             "disabled"
#endif
    );

    s_new_devices = xQueueCreate(16, sizeof(uint8_t));
    if (s_new_devices == NULL) {
        ESP_LOGE(TAG, "failed to allocate device queue");
        return;
    }

    TaskHandle_t host_task = NULL;
    BaseType_t created = xTaskCreate(host_daemon_task, "usb-host", 4096,
                                     xTaskGetCurrentTaskHandle(), 2, &host_task);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create USB host task");
        return;
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
        ESP_LOGE(TAG, "USB host startup timed out");
        return;
    }

    usb_host_lib_info_t lib_info = {0};
    if (usb_host_lib_info(&lib_info) != ESP_OK) {
        ESP_LOGE(TAG, "USB host did not install");
        return;
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };

    usb_host_client_handle_t client = NULL;
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &client));
    ESP_LOGI(TAG, "probe client registered; connect hub + USB microphone now");

    // Catch a device that completed enumeration just before the client registered.
    uint8_t existing[16] = {0};
    int existing_count = 0;
    if (usb_host_device_addr_list_fill(16, existing, &existing_count) == ESP_OK) {
        for (int i = 0; i < existing_count; ++i) {
            inspect_device(client, existing[i]);
        }
    }

    while (true) {
        esp_err_t err = usb_host_client_handle_events(client, pdMS_TO_TICKS(100));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "client event loop failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(250));
        }

        uint8_t address = 0;
        while (xQueueReceive(s_new_devices, &address, 0) == pdTRUE) {
            inspect_device(client, address);
        }
    }
}
