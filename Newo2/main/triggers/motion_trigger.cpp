#include "motion_trigger.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "newo2_camera.h"
#include "newo2_event.h"

namespace MotionTrigger {
namespace {
constexpr const char *TAG = "motion_trigger";
constexpr uint32_t kIntervalMs = 400;
constexpr uint8_t kPixelDiffThreshold = 30;
constexpr float kTriggerPercent = 10.0f;
constexpr float kClearPercent = 7.0f;
constexpr int kSampleStep = 2;
constexpr int kWarmupFrames = 4;
constexpr int kConfirmFrames = 2;
constexpr int kClearFrames = 2;
volatile bool g_enabled = true;
uint8_t *g_current = nullptr;
uint8_t *g_previous = nullptr;

float changed_percent() {
    size_t changed = 0;
    size_t sampled = 0;
    for (int y = 0; y < Newo2Camera::kMotionHeight; y += kSampleStep) {
        for (int x = 0; x < Newo2Camera::kMotionWidth; x += kSampleStep) {
            const size_t i = static_cast<size_t>(y) * Newo2Camera::kMotionWidth + x;
            const int diff = std::abs(static_cast<int>(g_current[i]) - static_cast<int>(g_previous[i]));
            if (diff >= kPixelDiffThreshold) ++changed;
            ++sampled;
        }
    }
    return sampled ? (100.0f * static_cast<float>(changed) / static_cast<float>(sampled)) : 0.0f;
}

void task(void *) {
    int warmup = 0;
    int confirm = 0;
    int clear = 0;
    bool active = false;
    bool have_previous = false;
    for (;;) {
        if (!g_enabled || !Newo2Camera::enabled()) {
            warmup = confirm = clear = 0;
            active = false;
            have_previous = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!Newo2Camera::capture_motion_luma(g_current, Newo2Camera::kMotionPixels)) {
            vTaskDelay(pdMS_TO_TICKS(kIntervalMs));
            continue;
        }
        if (!have_previous) {
            memcpy(g_previous, g_current, Newo2Camera::kMotionPixels);
            have_previous = true;
            vTaskDelay(pdMS_TO_TICKS(kIntervalMs));
            continue;
        }
        const float percent = changed_percent();
        memcpy(g_previous, g_current, Newo2Camera::kMotionPixels);
        if (warmup < kWarmupFrames) {
            ++warmup;
            vTaskDelay(pdMS_TO_TICKS(kIntervalMs));
            continue;
        }

        if (!active) {
            confirm = percent >= kTriggerPercent ? confirm + 1 : 0;
            if (confirm >= kConfirmFrames) {
                active = true;
                clear = 0;
                Newo2Events::Event event = {};
                event.type = Newo2Events::Type::MOTION_DETECTED;
                event.confidence = std::min(percent / 100.0f, 1.0f);
                event.sequence = Newo2Events::next_sequence();
                strlcpy(event.source, "motion", sizeof(event.source));
                Newo2Events::publish(event, 20);
                ESP_LOGI(TAG, "motion seq=%lu changed=%.1f%%", static_cast<unsigned long>(event.sequence), static_cast<double>(percent));
            }
        } else {
            clear = percent <= kClearPercent ? clear + 1 : 0;
            if (clear >= kClearFrames) {
                active = false;
                confirm = clear = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kIntervalMs));
    }
}
}  // namespace

bool begin() {
    g_current = static_cast<uint8_t *>(heap_caps_malloc(Newo2Camera::kMotionPixels, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_previous = static_cast<uint8_t *>(heap_caps_malloc(Newo2Camera::kMotionPixels, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!g_current || !g_previous) return false;
    return xTaskCreate(task, "motion_trigger", 4096, nullptr, 3, nullptr) == pdPASS;
}

void set_enabled(bool enabled) { g_enabled = enabled; }
bool enabled() { return g_enabled; }
}  // namespace MotionTrigger
