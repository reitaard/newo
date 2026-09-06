#include "uac2_speaker_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "speaker";

#define D07_VID 0x3302
#define D07_PID 0x3395

#define UAC2_CUR_REQUEST             0x01
#define UAC2_CLOCK_SAM_FREQ_CONTROL  0x01
#define TEST_RATE_HZ                 48000U
#define TEST_TONE_HZ                 1000U
#define TEST_AMPLITUDE               512
#define TEST_PACKETS_PER_TRANSFER    8
#define TEST_TRANSFER_COUNT          4
#define TEST_PACKET_BYTES            192U  // 48 frames * stereo * 16-bit
#define TEST_TRANSFER_BYTES          (TEST_PACKET_BYTES * TEST_PACKETS_PER_TRANSFER)
#define TEST_TOTAL_BATCHES           96U   // 96 * 8 ms = 768 ms

// One complete 1 kHz cycle at 48 kHz, amplitude about -36 dBFS.
static const int16_t kTone48[48] = {
      0,   67,  133,  196,  256,  312,  362,  406,
    443,  473,  495,  508,  512,  508,  495,  473,
    443,  406,  362,  312,  256,  196,  133,   67,
      0,  -67, -133, -196, -256, -312, -362, -406,
   -443, -473, -495, -508, -512, -508, -495, -473,
   -443, -406, -362, -312, -256, -196, -133,  -67,
};

typedef struct {
    SemaphoreHandle_t done;
    usb_transfer_status_t status;
    int actual_num_bytes;
} ctrl_wait_t;

typedef struct {
    uint8_t iface;
    uint8_t alt;
    uint8_t ep;
    uint8_t clock_id;
    uint16_t mps;
    bool valid;
} playback_candidate_t;

typedef struct {
    SemaphoreHandle_t done;
    uint32_t submitted_batches;
    uint32_t completed_batches;
    uint32_t completed_packets;
    uint32_t transfer_errors;
    uint32_t packet_errors;
    uint32_t submitted_bytes;
    uint32_t completed_bytes;
    uint32_t active_transfers;
} tone_state_t;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void ctrl_done_cb(usb_transfer_t *transfer)
{
    ctrl_wait_t *wait = (ctrl_wait_t *)transfer->context;
    if (wait == NULL) {
        return;
    }
    wait->status = transfer->status;
    wait->actual_num_bytes = transfer->actual_num_bytes;
    xSemaphoreGive(wait->done);
}

static esp_err_t control_request(usb_host_client_handle_t client,
                                 usb_device_handle_t dev,
                                 uint8_t bm_request_type,
                                 uint8_t request,
                                 uint16_t value,
                                 uint16_t index,
                                 void *data,
                                 uint16_t length)
{
    ctrl_wait_t wait = {
        .done = xSemaphoreCreateBinary(),
        .status = USB_TRANSFER_STATUS_ERROR,
        .actual_num_bytes = 0,
    };
    if (wait.done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    usb_transfer_t *transfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + length, 0, &transfer);
    if (err != ESP_OK) {
        vSemaphoreDelete(wait.done);
        return err;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)transfer->data_buffer;
    setup->bmRequestType = bm_request_type;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = length;

    const bool direction_in = (bm_request_type & USB_BM_REQUEST_TYPE_DIR_IN) != 0;
    if (!direction_in && length > 0 && data != NULL) {
        memcpy(transfer->data_buffer + sizeof(usb_setup_packet_t), data, length);
    }

    transfer->device_handle = dev;
    transfer->bEndpointAddress = 0;
    transfer->callback = ctrl_done_cb;
    transfer->context = &wait;
    transfer->timeout_ms = 1000;
    transfer->num_bytes = sizeof(usb_setup_packet_t) + length;

    err = usb_host_transfer_submit_control(client, transfer);
    if (err == ESP_OK) {
        if (xSemaphoreTake(wait.done, pdMS_TO_TICKS(1500)) != pdTRUE) {
            ESP_LOGE(TAG, "control request 0x%02x timed out", request);
            err = ESP_ERR_TIMEOUT;
        } else if (wait.status != USB_TRANSFER_STATUS_COMPLETED) {
            ESP_LOGE(TAG, "control request 0x%02x failed status=%d actual=%d",
                     request, wait.status, wait.actual_num_bytes);
            err = ESP_FAIL;
        } else if (direction_in && length > 0 && data != NULL) {
            memcpy(data, transfer->data_buffer + sizeof(usb_setup_packet_t), length);
        }
    }

    usb_host_transfer_free(transfer);
    vSemaphoreDelete(wait.done);
    return err;
}

static esp_err_t uac2_set_clock_rate(usb_host_client_handle_t client,
                                     usb_device_handle_t dev,
                                     uint8_t control_iface,
                                     uint8_t clock_id,
                                     uint32_t rate)
{
    uint8_t payload[4];
    put_le32(payload, rate);
    return control_request(client, dev,
                           USB_BM_REQUEST_TYPE_DIR_OUT |
                           USB_BM_REQUEST_TYPE_TYPE_CLASS |
                           USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                           UAC2_CUR_REQUEST,
                           (uint16_t)UAC2_CLOCK_SAM_FREQ_CONTROL << 8,
                           ((uint16_t)clock_id << 8) | control_iface,
                           payload, sizeof(payload));
}

static esp_err_t uac2_get_clock_rate(usb_host_client_handle_t client,
                                     usb_device_handle_t dev,
                                     uint8_t control_iface,
                                     uint8_t clock_id,
                                     uint32_t *rate)
{
    uint8_t payload[4] = {0};
    esp_err_t err = control_request(client, dev,
                                    USB_BM_REQUEST_TYPE_DIR_IN |
                                    USB_BM_REQUEST_TYPE_TYPE_CLASS |
                                    USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                                    UAC2_CUR_REQUEST,
                                    (uint16_t)UAC2_CLOCK_SAM_FREQ_CONTROL << 8,
                                    ((uint16_t)clock_id << 8) | control_iface,
                                    payload, sizeof(payload));
    if (err == ESP_OK && rate != NULL) {
        *rate = le32(payload);
    }
    return err;
}

static esp_err_t set_interface(usb_host_client_handle_t client,
                               usb_device_handle_t dev,
                               uint8_t iface,
                               uint8_t alt)
{
    return control_request(client, dev,
                           USB_BM_REQUEST_TYPE_DIR_OUT |
                           USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                           USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                           USB_B_REQUEST_SET_INTERFACE,
                           alt, iface, NULL, 0);
}

static playback_candidate_t find_pcm16_stereo_playback(const usb_config_desc_t *cfg)
{
    playback_candidate_t result = {0};
    if (cfg == NULL) {
        return result;
    }

    const uint8_t *raw = (const uint8_t *)cfg;
    const size_t total = cfg->wTotalLength;

    uint8_t iface = 0;
    uint8_t alt = 0;
    uint8_t iface_class = 0;
    uint8_t iface_subclass = 0;
    uint8_t iface_protocol = 0;
    uint8_t terminal_link = 0;
    uint8_t channels = 0;
    uint8_t subslot = 0;
    uint8_t bits = 0;
    uint8_t streaming_terminal = 0;
    uint8_t clock_id = 0;

    for (size_t pos = 0; pos + 2 <= total;) {
        const uint8_t *d = raw + pos;
        const uint8_t len = d[0];
        const uint8_t type = d[1];
        if (len < 2 || pos + len > total) {
            break;
        }

        if (type == 0x04 && len >= 9) {
            iface = d[2];
            alt = d[3];
            iface_class = d[5];
            iface_subclass = d[6];
            iface_protocol = d[7];
            terminal_link = 0;
            channels = 0;
            subslot = 0;
            bits = 0;
        } else if (type == 0x24 && len >= 3 && iface_class == 0x01 && iface_protocol == 0x20) {
            const uint8_t subtype = d[2];

            // UAC2 Input Terminal representing the USB streaming source.
            if (iface_subclass == 0x01 && subtype == 0x02 && len >= 17 && le16(d + 4) == 0x0101) {
                streaming_terminal = d[3];
                clock_id = d[7];
            }

            // UAC2 AudioStreaming AS_GENERAL.
            if (iface_subclass == 0x02 && subtype == 0x01 && len >= 16) {
                terminal_link = d[3];
                const uint8_t format_type = d[5];
                const uint32_t formats = le32(d + 6);
                channels = d[10];
                if (format_type != 0x01 || (formats & 0x00000001U) == 0) {
                    channels = 0;
                }
            }

            // UAC2 Type-I format descriptor.
            if (iface_subclass == 0x02 && subtype == 0x02 && len >= 6 && d[3] == 0x01) {
                subslot = d[4];
                bits = d[5];
            }
        } else if (type == 0x05 && len >= 7 &&
                   iface_class == 0x01 && iface_subclass == 0x02 && iface_protocol == 0x20) {
            const uint8_t ep = d[2];
            const uint8_t attr = d[3];
            const uint16_t mps = le16(d + 4) & 0x07ff;
            const bool out_iso = ((ep & 0x80) == 0) && ((attr & 0x03) == 0x01);
            if (out_iso && channels == 2 && subslot == 2 && bits == 16 &&
                terminal_link != 0 && terminal_link == streaming_terminal && clock_id != 0 &&
                mps >= TEST_PACKET_BYTES) {
                result.iface = iface;
                result.alt = alt;
                result.ep = ep;
                result.clock_id = clock_id;
                result.mps = mps;
                result.valid = true;
                return result;
            }
        }

        pos += len;
    }

    return result;
}

static void fill_tone_transfer(usb_transfer_t *transfer)
{
    int16_t *samples = (int16_t *)transfer->data_buffer;
    for (int packet = 0; packet < TEST_PACKETS_PER_TRANSFER; ++packet) {
        for (int frame = 0; frame < 48; ++frame) {
            const int16_t sample = kTone48[frame];
            *samples++ = sample;
            *samples++ = sample;
        }
        transfer->isoc_packet_desc[packet].num_bytes = TEST_PACKET_BYTES;
    }
    transfer->num_bytes = TEST_TRANSFER_BYTES;
}

static void tone_done_cb(usb_transfer_t *transfer)
{
    tone_state_t *state = (tone_state_t *)transfer->context;
    if (state == NULL) {
        return;
    }

    state->completed_batches++;
    state->completed_bytes += transfer->actual_num_bytes;

    bool transfer_ok = transfer->status == USB_TRANSFER_STATUS_COMPLETED;
    if (!transfer_ok) {
        state->transfer_errors++;
    }

    for (int i = 0; i < TEST_PACKETS_PER_TRANSFER; ++i) {
        if (transfer->isoc_packet_desc[i].status == USB_TRANSFER_STATUS_COMPLETED) {
            state->completed_packets++;
        } else {
            state->packet_errors++;
            transfer_ok = false;
        }
    }

    if (transfer_ok && state->submitted_batches < TEST_TOTAL_BATCHES) {
        state->submitted_batches++;
        state->submitted_bytes += TEST_TRANSFER_BYTES;
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err == ESP_OK) {
            return;
        }
        ESP_LOGE(TAG, "tone resubmit failed: %s", esp_err_to_name(err));
        state->transfer_errors++;
    }

    if (state->active_transfers > 0) {
        state->active_transfers--;
    }
    if (state->active_transfers == 0) {
        xSemaphoreGive(state->done);
    }
}

static esp_err_t run_tone(usb_device_handle_t dev, uint8_t endpoint)
{
    tone_state_t state = {
        .done = xSemaphoreCreateBinary(),
    };
    if (state.done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    usb_transfer_t *transfers[TEST_TRANSFER_COUNT] = {0};
    esp_err_t err = ESP_OK;

    for (int i = 0; i < TEST_TRANSFER_COUNT; ++i) {
        err = usb_host_transfer_alloc(TEST_TRANSFER_BYTES, TEST_PACKETS_PER_TRANSFER, &transfers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tone transfer allocation %d failed: %s", i, esp_err_to_name(err));
            break;
        }
        transfers[i]->device_handle = dev;
        transfers[i]->bEndpointAddress = endpoint;
        transfers[i]->callback = tone_done_cb;
        transfers[i]->context = &state;
        transfers[i]->timeout_ms = 1000;
        fill_tone_transfer(transfers[i]);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "tone start 1000Hz stereo PCM16 48000Hz amplitude=%d (~-36dBFS) duration=768ms",
                 TEST_AMPLITUDE);
        for (int i = 0; i < TEST_TRANSFER_COUNT; ++i) {
            state.submitted_batches++;
            state.submitted_bytes += TEST_TRANSFER_BYTES;
            state.active_transfers++;
            err = usb_host_transfer_submit(transfers[i]);
            if (err != ESP_OK) {
                state.submitted_batches--;
                state.submitted_bytes -= TEST_TRANSFER_BYTES;
                state.active_transfers--;
                state.transfer_errors++;
                ESP_LOGE(TAG, "initial tone submit %d failed: %s", i, esp_err_to_name(err));
                break;
            }
        }
    }

    if (state.active_transfers > 0) {
        if (xSemaphoreTake(state.done, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGE(TAG, "tone completion timed out; halting endpoint 0x%02x", endpoint);
            state.transfer_errors++;
            usb_host_endpoint_halt(dev, endpoint);
            usb_host_endpoint_flush(dev, endpoint);
            usb_host_endpoint_clear(dev, endpoint);
            xSemaphoreTake(state.done, pdMS_TO_TICKS(1000));
            err = ESP_ERR_TIMEOUT;
        }
    }

    ESP_LOGI(TAG,
             "tone done submitted-batches=%lu completed-batches=%lu completed-packets=%lu submitted-bytes=%lu completed-bytes=%lu transfer-errors=%lu packet-errors=%lu",
             (unsigned long)state.submitted_batches,
             (unsigned long)state.completed_batches,
             (unsigned long)state.completed_packets,
             (unsigned long)state.submitted_bytes,
             (unsigned long)state.completed_bytes,
             (unsigned long)state.transfer_errors,
             (unsigned long)state.packet_errors);

    for (int i = 0; i < TEST_TRANSFER_COUNT; ++i) {
        if (transfers[i] != NULL) {
            usb_host_transfer_free(transfers[i]);
        }
    }
    vSemaphoreDelete(state.done);

    if (err == ESP_OK && (state.transfer_errors != 0 || state.packet_errors != 0)) {
        err = ESP_FAIL;
    }
    return err;
}

esp_err_t newo_uac2_speaker_test(usb_host_client_handle_t client,
                                 usb_device_handle_t dev,
                                 const usb_device_desc_t *dev_desc,
                                 const usb_config_desc_t *cfg)
{
    if (client == NULL || dev == NULL || dev_desc == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Never emit audio into an arbitrary USB device. This probe is deliberately
    // locked to the physical D07 that was identified on the bench.
    if (dev_desc->idVendor != D07_VID || dev_desc->idProduct != D07_PID) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const playback_candidate_t playback = find_pcm16_stereo_playback(cfg);
    if (!playback.valid) {
        ESP_LOGW(TAG, "D07 found but no safe PCM16 stereo OUT alternate matched");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG,
             "D07 playback selected iface=%u alt=%u ep=0x%02x MPS=%u clock=%u PCM16 stereo",
             playback.iface, playback.alt, playback.ep, playback.mps, playback.clock_id);

    ESP_LOGI(TAG, "setting UAC2 clock source %u to %uHz", playback.clock_id, TEST_RATE_HZ);
    esp_err_t set_rate_err = uac2_set_clock_rate(client, dev, 0, playback.clock_id, TEST_RATE_HZ);
    if (set_rate_err != ESP_OK) {
        ESP_LOGW(TAG, "clock SET_CUR failed: %s; checking current rate", esp_err_to_name(set_rate_err));
    }

    uint32_t actual_rate = 0;
    esp_err_t err = uac2_get_clock_rate(client, dev, 0, playback.clock_id, &actual_rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clock GET_CUR failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "clock GET_CUR=%luHz", (unsigned long)actual_rate);
    if (actual_rate != TEST_RATE_HZ) {
        ESP_LOGE(TAG, "refusing tone: expected 48000Hz, device reports %luHz", (unsigned long)actual_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = usb_host_interface_claim(client, dev, playback.iface, playback.alt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "claim interface %u alt %u failed: %s",
                 playback.iface, playback.alt, esp_err_to_name(err));
        return err;
    }

    err = set_interface(client, dev, playback.iface, playback.alt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SET_INTERFACE %u/%u failed: %s",
                 playback.iface, playback.alt, esp_err_to_name(err));
        usb_host_interface_release(client, dev, playback.iface);
        return err;
    }

    err = run_tone(dev, playback.ep);

    esp_err_t alt0_err = set_interface(client, dev, playback.iface, 0);
    if (alt0_err != ESP_OK) {
        ESP_LOGW(TAG, "SET_INTERFACE %u/0 failed: %s",
                 playback.iface, esp_err_to_name(alt0_err));
    }

    esp_err_t release_err = usb_host_interface_release(client, dev, playback.iface);
    if (release_err != ESP_OK) {
        ESP_LOGW(TAG, "release interface %u failed: %s",
                 playback.iface, esp_err_to_name(release_err));
        if (err == ESP_OK) {
            err = release_err;
        }
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "D07 speaker transport PASS");
    } else {
        ESP_LOGE(TAG, "D07 speaker transport FAIL: %s", esp_err_to_name(err));
    }
    return err;
}
