#include "newo2_media.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "newo2_camera.h"
#include "newo2_event.h"
#include "newo2_network.h"
#include "newo2_storage.h"

namespace Newo2Media {
namespace {
constexpr const char *TAG = "newo2_media";
constexpr uint8_t kTargetFps = 20;
constexpr uint8_t kFallbackFps = 15;
SemaphoreHandle_t g_mutex = nullptr;
bool g_streaming = false;
bool g_recording = false;
uint8_t g_fps = kTargetFps;
uint32_t g_duration_seconds = 0;
int64_t g_record_started_us = 0;
uint32_t g_frames = 0;
uint32_t g_dropped = 0;
uint32_t g_overruns = 0;
uint32_t g_good_frames = 0;
char g_record_request_id[40] = {};

void finish_recording(bool success, const char *reason) {
    char request_id[40] = {};
    uint32_t frames = 0, dropped = 0;
    int64_t started = 0;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) != pdTRUE || !g_recording) return;
    g_recording = false;
    strlcpy(request_id, g_record_request_id, sizeof(request_id));
    frames = g_frames; dropped = g_dropped; started = g_record_started_us;
    g_record_request_id[0] = '\0';
    xSemaphoreGive(g_mutex);

    size_t bytes = 0;
    const bool closed = Newo2Storage::end_video(&bytes);
    const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);
    Newo2Network::send_record_result(request_id, success && closed, frames, dropped, bytes, elapsed_ms,
                                     reason ? reason : (closed ? "complete" : "sd_close_failed"));
}

void media_task(void *) {
    int64_t next_us = esp_timer_get_time();
    for (;;) {
        bool stream = false, record = false;
        uint32_t duration = 0;
        int64_t started = 0;
        uint8_t fps = kTargetFps;
        if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
            stream = g_streaming; record = g_recording; duration = g_duration_seconds;
            started = g_record_started_us; fps = record ? kTargetFps : g_fps;
            xSemaphoreGive(g_mutex);
        }
        if (!stream && !record) { next_us = esp_timer_get_time(); vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (record && duration && esp_timer_get_time() - started >= static_cast<int64_t>(duration) * 1000000LL) {
            finish_recording(true, "duration_complete");
            continue;
        }

        const int64_t capture_started = esp_timer_get_time();
        Newo2Camera::Snapshot frame;
        const bool captured = Newo2Camera::capture_video_frame(frame);
        bool sd_ok = true;
        if (captured && record) sd_ok = Newo2Storage::append_video_frame(frame.jpeg, frame.len);
        bool queued = false;
        if (captured) queued = Newo2Network::send_video_frame(frame.jpeg, frame.len, frame.width, frame.height,
                                                               frame.sequence, fps, stream, record);
        const uint32_t elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - capture_started) / 1000);

        if (xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
            const uint32_t budget_ms = 1000 / kTargetFps;
            if (record) {
                if (captured && sd_ok) ++g_frames;
                else ++g_dropped;

                // Recording is fixed at 20 FPS. Network delivery is queued by
                // Newo2Network and must never down-clock the SD recording path.
                g_fps = kTargetFps;
                if (!captured || elapsed_ms > budget_ms) {
                    ++g_overruns;
                    g_good_frames = 0;
                    if (g_overruns == 5 || (g_overruns > 5 && g_overruns % 100 == 0)) {
                        ESP_LOGW(TAG, "recording holding 20 fps under local pressure frame_ms=%lu",
                                 static_cast<unsigned long>(elapsed_ms));
                    }
                } else {
                    g_overruns = 0;
                    ++g_good_frames;
                }
                fps = kTargetFps;
            } else {
                // Live stream may still adapt when the local capture path or
                // outbound queue cannot keep up. This policy never applies to recording.
                if (!captured || elapsed_ms > 1000 / g_fps || (stream && !queued)) {
                    ++g_overruns;
                    g_good_frames = 0;
                } else {
                    g_overruns = 0;
                    ++g_good_frames;
                }
                if (g_fps == kTargetFps && g_overruns >= 5) {
                    g_fps = kFallbackFps;
                    g_overruns = 0;
                    ESP_LOGW(TAG, "stream falling back to 15 fps");
                }
                if (g_fps == kFallbackFps && g_good_frames >= 100) {
                    g_fps = kTargetFps;
                    g_good_frames = 0;
                    ESP_LOGI(TAG, "stream restoring 20 fps");
                }
                fps = g_fps;
            }
            xSemaphoreGive(g_mutex);
        }
        Newo2Camera::release_snapshot(frame);
        if (record && !sd_ok) { finish_recording(false, "sd_write_failed"); continue; }

        next_us += 1000000 / fps;
        const int64_t delay_us = next_us - esp_timer_get_time();
        if (delay_us > 1000) vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(delay_us / 1000)));
        else next_us = esp_timer_get_time();
    }
}
}  // namespace

bool begin() {
    g_mutex = xSemaphoreCreateMutex();
    return g_mutex && xTaskCreate(media_task, "newo2_media", 6144, nullptr, 5, nullptr) == pdPASS;
}

bool set_streaming(bool enabled, const char *request_id) {
    if (!g_mutex) return false;
    if (enabled && !Newo2Camera::enabled() && !Newo2Camera::set_enabled(true)) return false;
    xSemaphoreTake(g_mutex, portMAX_DELAY); g_streaming = enabled; xSemaphoreGive(g_mutex);
    Newo2Network::send_media_ack(request_id, "stream", enabled, true, g_fps);
    return true;
}

bool start_recording(uint32_t duration_seconds, const char *request_id) {
    if (!g_mutex || !request_id || !request_id[0]) return false;
    if (!Newo2Camera::enabled() && !Newo2Camera::set_enabled(true)) return false;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (g_recording) { xSemaphoreGive(g_mutex); return false; }
    char path[128] = {};
    const bool opened = Newo2Storage::begin_video(Newo2Events::next_sequence(), path, sizeof(path));
    if (!opened) { xSemaphoreGive(g_mutex); return false; }
    g_recording = true; g_duration_seconds = duration_seconds; g_record_started_us = esp_timer_get_time();
    g_frames = g_dropped = 0; g_fps = kTargetFps; g_overruns = g_good_frames = 0;
    strlcpy(g_record_request_id, request_id, sizeof(g_record_request_id));
    xSemaphoreGive(g_mutex);
    Newo2Network::send_media_ack(request_id, "record", true, true, kTargetFps);
    return true;
}

bool stop_recording(const char *request_id) {
    if (!g_recording) { Newo2Network::send_media_ack(request_id, "record", false, false, g_fps); return false; }
    finish_recording(true, "stopped");
    Newo2Network::send_media_ack(request_id, "record", false, true, g_fps);
    return true;
}

bool streaming() { return g_streaming; }
bool recording() { return g_recording; }
uint8_t effective_fps() { return g_recording ? kTargetFps : g_fps; }
}  // namespace Newo2Media
