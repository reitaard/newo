#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "motion_snapshot.h"
#include "motion_trigger.h"
#include "newo2_camera.h"
#include "newo2_event.h"
#include "newo2_network.h"
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
                if (!event.enabled) Newo2Network::send_status(Newo2Camera::enabled(), MotionTrigger::enabled());
                break;
            }
            case Newo2Events::Type::MOTION_SET:
                MotionTrigger::set_enabled(event.enabled);
                Newo2Network::send_control_ack(event.request_id, "motion", MotionTrigger::enabled(), true);
                break;
            case Newo2Events::Type::STATUS_REQUEST:
                Newo2Network::send_status(Newo2Camera::enabled(), MotionTrigger::enabled());
                break;
            case Newo2Events::Type::SNAPSHOT_REQUEST:
                if (!MotionSnapshotSkill::enqueue(event))
                    Newo2Network::send_snapshot_result(event.request_id, event.source, event.sequence, 0, false, false, false);
                break;
            case Newo2Events::Type::MOTION_DETECTED:
                Newo2Network::send_motion_detected(event.sequence, event.confidence);
                if (!MotionSnapshotSkill::enqueue(event)) ESP_LOGW(TAG, "snapshot queue full for motion event");
                break;
        }
    }
}
}  // namespace

extern "C" void app_main() {
    ESP_LOGI(TAG, "Newo2 production v1 boot");
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);
    if (!Newo2Events::begin()) ESP_LOGE(TAG, "event bus init failed");
    Newo2Storage::begin();  // best effort: upload still works if SD is absent.
    ESP_ERROR_CHECK(Newo2Camera::begin() ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(MotionSnapshotSkill::begin() ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(MotionTrigger::begin() ? ESP_OK : ESP_FAIL);
    Newo2Network::begin();  // best effort: local motion + SD still work offline.
    ESP_ERROR_CHECK(xTaskCreate(router_task, "newo2_router", 4096, nullptr, 5, nullptr) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "ready: camera=OFF motion=ON; enable camera through VPS control before capture");
}
