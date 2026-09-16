#include "newo2_camera.h"

#include <algorithm>
#include <cstring>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace Newo2Camera {
namespace {
constexpr const char *TAG = "newo2_camera";
constexpr uint8_t kDefaultVideoJpegQuality = 12;
constexpr uint8_t kDefaultPhotoJpegQuality = 10;
constexpr size_t kJpegEoiScanBytes = 4096;
constexpr framesize_t kDefaultVideoFrameSize = FRAMESIZE_VGA;
constexpr framesize_t kDefaultSnapshotFrameSize = FRAMESIZE_SVGA;
constexpr int CAM_PIN_PWDN = -1;
constexpr int CAM_PIN_RESET = -1;
constexpr int CAM_PIN_XCLK = 15;
constexpr int CAM_PIN_SIOD = 4;
constexpr int CAM_PIN_SIOC = 5;
constexpr int CAM_PIN_D7 = 16;
constexpr int CAM_PIN_D6 = 17;
constexpr int CAM_PIN_D5 = 18;
constexpr int CAM_PIN_D4 = 12;
constexpr int CAM_PIN_D3 = 10;
constexpr int CAM_PIN_D2 = 8;
constexpr int CAM_PIN_D1 = 9;
constexpr int CAM_PIN_D0 = 11;
constexpr int CAM_PIN_VSYNC = 6;
constexpr int CAM_PIN_HREF = 7;
constexpr int CAM_PIN_PCLK = 13;

SemaphoreHandle_t g_mutex = nullptr;
bool g_initialized = false;
volatile bool g_enabled = false;
uint16_t g_sensor_pid = 0;
framesize_t g_video_frame_size = kDefaultVideoFrameSize;
framesize_t g_photo_frame_size = kDefaultSnapshotFrameSize;
uint8_t g_video_quality = kDefaultVideoJpegQuality;
uint8_t g_photo_quality = kDefaultPhotoJpegQuality;
framesize_t g_profile = kDefaultVideoFrameSize;
uint32_t g_snapshot_sequence = 0;

size_t sanitized_jpeg_len(const uint8_t *data, size_t len) {
    if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return 0;
    const size_t floor = len > kJpegEoiScanBytes ? len - kJpegEoiScanBytes : 2;
    for (size_t i = len - 1; i > floor; --i) {
        if (data[i - 1] == 0xFF && data[i] == 0xD9) return i + 1;
    }
    return 0;
}

bool switch_profile_while_guarded(framesize_t profile, uint8_t quality) {
    if (g_profile == profile) return true;
    sensor_t *sensor = esp_camera_sensor_get();
    const bool switched = sensor && sensor->set_framesize(sensor, profile) == 0;
    if (switched) {
        sensor->set_quality(sensor, quality);
        g_profile = profile;
    }
    if (!switched) ESP_LOGW(TAG, "OV3660 frame-size switch failed profile=%d", static_cast<int>(profile));
    if (switched) {
        // Discard the first frame after an OV3660 mode change. With the v6
        // double-buffer/latest-frame setup it may still belong to the previous
        // profile.
        camera_fb_t *stale = esp_camera_fb_get();
        if (stale) esp_camera_fb_return(stale);
    }
    return switched;
}

const char *frame_size_name(framesize_t size) {
    switch (size) {
        case FRAMESIZE_QVGA: return "qvga";
        case FRAMESIZE_VGA: return "vga";
        case FRAMESIZE_SVGA: return "svga";
        case FRAMESIZE_XGA: return "xga";
        default: return "unknown";
    }
}

bool parse_frame_size(const char *target, const char *name, framesize_t &size) {
    if (!target || !name) return false;
    if (strcmp(target, "video") == 0) {
        if (strcmp(name, "qvga") == 0) size = FRAMESIZE_QVGA;
        else if (strcmp(name, "vga") == 0) size = FRAMESIZE_VGA;
        else return false;
    } else if (strcmp(target, "photo") == 0) {
        if (strcmp(name, "vga") == 0) size = FRAMESIZE_VGA;
        else if (strcmp(name, "svga") == 0) size = FRAMESIZE_SVGA;
        else if (strcmp(name, "xga") == 0) size = FRAMESIZE_XGA;
        else return false;
    } else return false;
    return true;
}

void persist_settings() {
    nvs_handle_t handle = 0;
    if (nvs_open("newo2cam", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "photo_res", static_cast<uint8_t>(g_photo_frame_size));
    nvs_set_u8(handle, "video_res", static_cast<uint8_t>(g_video_frame_size));
    nvs_set_u8(handle, "photo_q", g_photo_quality);
    nvs_set_u8(handle, "video_q", g_video_quality);
    nvs_commit(handle);
    nvs_close(handle);
}

void load_settings() {
    nvs_handle_t handle = 0;
    if (nvs_open("newo2cam", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t value = 0;
    if (nvs_get_u8(handle, "photo_res", &value) == ESP_OK &&
        (value == FRAMESIZE_VGA || value == FRAMESIZE_SVGA || value == FRAMESIZE_XGA)) g_photo_frame_size = static_cast<framesize_t>(value);
    if (nvs_get_u8(handle, "video_res", &value) == ESP_OK &&
        (value == FRAMESIZE_QVGA || value == FRAMESIZE_VGA)) g_video_frame_size = static_cast<framesize_t>(value);
    if (nvs_get_u8(handle, "photo_q", &value) == ESP_OK && value >= 4 && value <= 32) g_photo_quality = value;
    if (nvs_get_u8(handle, "video_q", &value) == ESP_OK && value >= 4 && value <= 32) g_video_quality = value;
    nvs_close(handle);
}

bool restore_video_profile_before_return(camera_fb_t *held_snapshot) {
    sensor_t *sensor = esp_camera_sensor_get();
    const bool switched = sensor && sensor->set_framesize(sensor, g_video_frame_size) == 0;
    if (switched) {
        sensor->set_quality(sensor, g_video_quality);
        g_profile = g_video_frame_size;
    } else {
        ESP_LOGW(TAG, "failed to restore video profile while photo framebuffer held");
    }

    if (held_snapshot) esp_camera_fb_return(held_snapshot);
    if (switched) {
        camera_fb_t *stale = esp_camera_fb_get();
        if (stale) esp_camera_fb_return(stale);
    }
    return switched;
}

}  // namespace

bool begin() {
    if (g_initialized) return true;
    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) return false;

    load_settings();
    camera_config_t config = {};
    config.pin_pwdn = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_d7 = CAM_PIN_D7;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d0 = CAM_PIN_D0;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_pclk = CAM_PIN_PCLK;
    config.xclk_freq_hz = 20'000'000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = g_video_frame_size;
    config.jpeg_quality = g_video_quality;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.sccb_i2c_port = 0;

    const esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return false;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        g_sensor_pid = sensor->id.PID;
        sensor->set_quality(sensor, g_video_quality);
        const int vflip_rc = sensor->set_vflip(sensor, 1);
        const int hmirror_rc = sensor->set_hmirror(sensor, 1);
        if (vflip_rc != 0 || hmirror_rc != 0) {
            ESP_LOGW(TAG, "OV3660 180-degree orientation failed vflip=%d hmirror=%d", vflip_rc, hmirror_rc);
        }
    }
    if (g_sensor_pid != 0x3660) {
        ESP_LOGW(TAG, "unexpected sensor PID=0x%04x", g_sensor_pid);
    }

    g_initialized = true;
    g_enabled = false;  // privacy-safe logical boot state; hardware stays warm.
    ESP_LOGI(TAG, "ready PID=0x%04x native JPEG VGA q=%u 2FB LATEST PSRAM DMA OFF orientation=180; logical camera OFF",
             g_sensor_pid, static_cast<unsigned>(g_video_quality));
    return true;
}

bool set_enabled(bool enabled) {
    if (!g_initialized || !g_mutex) return false;
    // This mutex is also the ON/OFF contract. An OFF ACK is not sent until any
    // in-flight capture has returned its framebuffer; captures re-check state
    // after acquiring this same mutex, so none can begin after the barrier.
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2500)) != pdTRUE) return false;
    g_enabled = enabled;
    xSemaphoreGive(g_mutex);
    ESP_LOGI(TAG, "logical camera %s", enabled ? "ON" : "OFF");
    return true;
}

bool enabled() { return g_enabled; }
uint16_t sensor_pid() { return g_sensor_pid; }

bool capture_snapshot(Snapshot &snapshot) {
    snapshot = {};
    if (!g_initialized || !g_enabled) return false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1500)) != pdTRUE) return false;
    if (!g_enabled) {
        xSemaphoreGive(g_mutex);
        return false;
    }

    bool ok = false;
    if (switch_profile_while_guarded(g_photo_frame_size, g_photo_quality)) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            const size_t clean_len = fb->format == PIXFORMAT_JPEG ? sanitized_jpeg_len(fb->buf, fb->len) : 0;
            if (clean_len) {
                uint8_t *copy = static_cast<uint8_t *>(
                    heap_caps_malloc(clean_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (copy) {
                    memcpy(copy, fb->buf, clean_len);
                    snapshot.jpeg = copy;
                    snapshot.len = clean_len;
                    snapshot.width = fb->width;
                    snapshot.height = fb->height;
                    snapshot.sequence = ++g_snapshot_sequence;
                    ok = true;
                }
            }
            if (!restore_video_profile_before_return(fb)) ok = false;
        } else {
            ESP_LOGW(TAG, "snapshot framebuffer unavailable");
            // No frame is held, so use the guarded transition back to VGA.
            switch_profile_while_guarded(g_video_frame_size, g_video_quality);
        }
    }

    xSemaphoreGive(g_mutex);
    if (!ok && snapshot.jpeg) release_snapshot(snapshot);
    if (ok) {
        ESP_LOGI(TAG, "snapshot seq=%lu %ux%u bytes=%u",
                 static_cast<unsigned long>(snapshot.sequence), snapshot.width, snapshot.height,
                 static_cast<unsigned>(snapshot.len));
    }
    return ok;
}

bool capture_video_frame(Snapshot &frame) {
    frame = {};
    if (!g_initialized || !g_enabled) return false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(250)) != pdTRUE) return false;
    if (!g_enabled) { xSemaphoreGive(g_mutex); return false; }

    bool ok = false;
    if (switch_profile_while_guarded(g_video_frame_size, g_video_quality)) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            const size_t clean_len = fb->format == PIXFORMAT_JPEG ? sanitized_jpeg_len(fb->buf, fb->len) : 0;
            if (clean_len) {
                uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(clean_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (copy) {
                    memcpy(copy, fb->buf, clean_len);
                    frame.jpeg = copy;
                    frame.len = clean_len;
                    frame.width = fb->width;
                    frame.height = fb->height;
                    frame.sequence = ++g_snapshot_sequence;
                    ok = true;
                }
            }
            esp_camera_fb_return(fb);
        }
    }
    xSemaphoreGive(g_mutex);
    return ok;
}

void release_snapshot(Snapshot &snapshot) {
    if (snapshot.jpeg) heap_caps_free(snapshot.jpeg);
    snapshot = {};
}

Settings settings() {
    return {frame_size_name(g_photo_frame_size), frame_size_name(g_video_frame_size), g_photo_quality, g_video_quality};
}

bool set_resolution(const char *target, const char *resolution) {
    framesize_t next = FRAMESIZE_INVALID;
    if (!parse_frame_size(target, resolution, next) || !g_mutex) return false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2500)) != pdTRUE) return false;
    if (strcmp(target, "photo") == 0) g_photo_frame_size = next; else g_video_frame_size = next;
    const bool applied = strcmp(target, "photo") == 0 || switch_profile_while_guarded(g_video_frame_size, g_video_quality);
    if (applied) persist_settings();
    xSemaphoreGive(g_mutex);
    return applied;
}

bool set_quality(const char *target, uint8_t quality) {
    if (!g_mutex || quality < 4 || quality > 32 || (strcmp(target, "photo") != 0 && strcmp(target, "video") != 0)) return false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(2500)) != pdTRUE) return false;
    if (strcmp(target, "photo") == 0) g_photo_quality = quality; else g_video_quality = quality;
    sensor_t *sensor = esp_camera_sensor_get();
    const bool applied = strcmp(target, "photo") == 0 || (sensor && sensor->set_quality(sensor, g_video_quality) == 0);
    if (applied) persist_settings();
    xSemaphoreGive(g_mutex);
    return applied;
}
}  // namespace Newo2Camera
