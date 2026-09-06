// Reuse the proven descriptor probe implementation in this experimental build,
// but replace app_main so USB client events keep being serviced while the
// speaker worker waits for asynchronous control/isochronous completions.
#define app_main descriptor_probe_original_app_main
#include "main.c"
#undef app_main

#include "uac2_speaker_test.h"

static void speaker_device_worker(void *arg)
{
    usb_host_client_handle_t client = (usb_host_client_handle_t)arg;

    while (true) {
        uint8_t address = 0;
        if (xQueueReceive(s_new_devices, &address, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Keep the original complete descriptor dump first.
        inspect_device(client, address);

        // Re-open only for the bounded D07 playback test. The tone helper is
        // VID/PID locked and returns NOT_SUPPORTED for all other USB devices.
        usb_device_handle_t dev = NULL;
        esp_err_t err = usb_host_device_open(client, address, &dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "speaker reopen address=%u failed: %s", address, esp_err_to_name(err));
            continue;
        }

        const usb_device_desc_t *dev_desc = NULL;
        const usb_config_desc_t *cfg = NULL;
        esp_err_t dev_desc_err = usb_host_get_device_descriptor(dev, &dev_desc);
        esp_err_t cfg_err = usb_host_get_active_config_descriptor(dev, &cfg);
        if (dev_desc_err == ESP_OK && cfg_err == ESP_OK) {
            err = newo_uac2_speaker_test(client, dev, dev_desc, cfg);
            if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGE(TAG, "speaker test address=%u failed: %s", address, esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "speaker descriptors address=%u unavailable dev=%s cfg=%s",
                     address, esp_err_to_name(dev_desc_err), esp_err_to_name(cfg_err));
        }

        err = usb_host_device_close(client, dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "speaker close address=%u failed: %s", address, esp_err_to_name(err));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "NEWO USB descriptor + D07 speaker probe");
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
    ESP_LOGI(TAG, "speaker probe client registered; D07 tone auto-runs once after enumeration");

    TaskHandle_t worker = NULL;
    created = xTaskCreate(speaker_device_worker, "speaker-worker", 6144,
                          client, 3, &worker);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create speaker worker");
        return;
    }

    // Catch devices that completed enumeration just before client registration.
    uint8_t existing[16] = {0};
    int existing_count = 0;
    if (usb_host_device_addr_list_fill(16, existing, &existing_count) == ESP_OK) {
        for (int i = 0; i < existing_count; ++i) {
            if (xQueueSend(s_new_devices, &existing[i], 0) != pdTRUE) {
                ESP_LOGW(TAG, "device queue full; dropped existing address=%u", existing[i]);
            }
        }
    }

    // This task exclusively services client callbacks. The worker performs all
    // potentially blocking descriptor/control/tone operations.
    while (true) {
        esp_err_t err = usb_host_client_handle_events(client, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "client event loop failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
