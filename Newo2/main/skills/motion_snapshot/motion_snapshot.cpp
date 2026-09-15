#include "motion_snapshot.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "newo2_camera.h"
#include "newo2_network.h"
#include "newo2_storage.h"

namespace MotionSnapshotSkill {
namespace {
constexpr const char *TAG = "motion_snapshot";
QueueHandle_t g_queue = nullptr;

void task(void *) {
    Newo2Events::Event event = {};
    for (;;) {
        if (xQueueReceive(g_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        Newo2Camera::Snapshot snapshot;
        const bool captured = Newo2Camera::capture_snapshot(snapshot);
        bool saved = false;
        bool uploaded = false;
        char path[160] = {};
        if (captured) {
            saved = Newo2Storage::save_snapshot(snapshot.jpeg, snapshot.len, snapshot.sequence, path, sizeof(path));
            uploaded = Newo2Network::upload_snapshot(snapshot.jpeg, snapshot.len, event.request_id,
                                                       event.source, snapshot.sequence);
        }
        Newo2Network::send_snapshot_result(event.request_id, event.source, captured ? snapshot.sequence : event.sequence,
                                            captured ? snapshot.len : 0, saved, uploaded, captured);
        if (!captured) ESP_LOGW(TAG, "snapshot failed source=%s", event.source);
        Newo2Camera::release_snapshot(snapshot);
    }
}
}  // namespace

bool begin() {
    g_queue = xQueueCreate(4, sizeof(Newo2Events::Event));
    if (!g_queue) return false;
    return xTaskCreate(task, "snapshot_skill", 6144, nullptr, 4, nullptr) == pdPASS;
}

bool enqueue(const Newo2Events::Event &event) {
    return g_queue && xQueueSend(g_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE;
}
}  // namespace MotionSnapshotSkill
