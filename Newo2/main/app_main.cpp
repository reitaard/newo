#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdlib>
#include <cstring>

#include "motion_snapshot.h"
#include "newo2_camera.h"
#include "newo2_console.h"
#include "newo2_event.h"
#include "newo2_network.h"
#include "newo2_media.h"
#include "newo2_storage.h"

namespace {
constexpr const char *TAG = "newo2";

void router_task(void *) {
    Newo2Events::Event event = {};
    for (;;) {
        if (!Newo2Events::receive(event, 1000)) continue;
        switch (event.type) {
            case Newo2Events::Type::CAMERA_SET: {
                const bool applied = Newo2Camera::set_enabled(event.enabled);
                Newo2Network::send_control_ack(event.request_id, "camera", Newo2Camera::enabled(), applied);
                if (!event.enabled) Newo2Network::send_status(Newo2Camera::enabled(), false);
                break;
            }
            case Newo2Events::Type::STATUS_REQUEST:
                Newo2Network::send_status(Newo2Camera::enabled(), false);
                break;
            case Newo2Events::Type::SNAPSHOT_REQUEST:
                if (!Newo2Camera::enabled()) Newo2Camera::set_enabled(true);
                if (!MotionSnapshotSkill::enqueue(event))
                    Newo2Network::send_snapshot_result(event.request_id, event.source, event.sequence, 0, false, false, false);
                break;
            case Newo2Events::Type::STREAM_SET:
                if (!Newo2Media::set_streaming(event.enabled, event.request_id))
                    Newo2Network::send_media_ack(event.request_id, "stream", Newo2Media::streaming(), false, Newo2Media::effective_fps());
                break;
            case Newo2Events::Type::RECORD_START:
                if (!Newo2Media::start_recording(event.duration_seconds, event.request_id))
                    Newo2Network::send_media_ack(event.request_id, "record", Newo2Media::recording(), false, Newo2Media::effective_fps());
                break;
            case Newo2Events::Type::RECORD_STOP:
                Newo2Media::stop_recording(event.request_id);
                break;
            case Newo2Events::Type::SETTINGS_SET: {
                bool applied = false;
                if (strcmp(event.setting, "resolution") == 0)
                    applied = Newo2Camera::set_resolution(event.source, event.value);
                else if (strcmp(event.setting, "quality") == 0)
                    applied = Newo2Camera::set_quality(event.source, static_cast<uint8_t>(atoi(event.value)));
                Newo2Network::send_camera_settings(event.request_id, applied);
                break;
            }
            case Newo2Events::Type::MOTION_SET:
            case Newo2Events::Type::MOTION_DETECTED:
                break; // Reserved for a future sensing firmware, not production phase 1.
        }
    }
}
}  // namespace

extern "C" void app_main() {
    ESP_LOGI(TAG, "Newo2 production v1 boot");
    // Never erase NVS automatically: it may contain the production Wi-Fi
    // credentials that an app-only update is deliberately preserving.
    ESP_ERROR_CHECK(nvs_flash_init());
    Newo2Console::begin();
    if (!Newo2Events::begin()) ESP_LOGE(TAG, "event bus init failed");
    Newo2Storage::begin();  // best effort: upload still works if SD is absent.
    ESP_ERROR_CHECK(Newo2Camera::begin() ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(MotionSnapshotSkill::begin() ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(Newo2Media::begin() ? ESP_OK : ESP_FAIL);
    Newo2Network::begin();  // best effort: SD photo capture still works offline.
    ESP_ERROR_CHECK(xTaskCreate(router_task, "newo2_router", 4096, nullptr, 5, nullptr) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "ready: camera=OFF sensing=excluded; enable camera through VPS control before capture");
}
