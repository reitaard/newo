#include "newo2_camera.h"

#include <algorithm>
#include <cstring>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace Newo2Camera {
namespace {
constexpr const char *TAG = "newo2_camera";
constexpr uint8_t kJpegQuality = 12;
constexpr size_t kJpegEoiScanBytes = 4096;
constexpr framesize_t kMotionFrameSize = FRAMESIZE_QVGA;
constexpr framesize_t kSnapshotFrameSize = FRAMESIZE_SVGA;
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
jpeg_dec_handle_t g_decoder = nullptr;
uint8_t *g_rgb565 = nullptr;
size_t g_rgb565_capacity = 0;
framesize_t g_profile = kMotionFrameSize;
uint32_t g_snapshot_sequence = 0;

size_t sanitized_jpeg_len(const uint8_t *data, size_t len) {
    if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return 0;
    const size_t floor = len > kJpegEoiScanBytes ? len - kJpegEoiScanBytes : 2;
    for (size_t i = len - 1; i > floor; --i) {
        if (data[i - 1] == 0xFF && data[i] == 0xD9) return i + 1;
    }
    return 0;
}

bool ensure_rgb565(size_t required) {
    if (g_rgb565 && g_rgb565_capacity >= required) return true;
    uint8_t *next = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(16, required, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!next) return false;
    if (g_rgb565) heap_caps_free(g_rgb565);
    g_rgb565 = next;
    g_rgb565_capacity = required;
    return true;
}

bool switch_profile_while_guarded(framesize_t profile) {
    if (g_profile == profile) return true;

    // One framebuffer + WHEN_EMPTY means checking out the current frame stops
    // the camera driver from starting another capture while OV3660 registers
    // are changed. This is intentionally the same safety pattern proven by
    // MomentScraper for detection/photo mode transitions.
    camera_fb_t *guard = esp_camera_fb_get();
    if (!guard) {
        ESP_LOGW(TAG, "could not guard camera before profile switch");
        return false;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    const bool switched = sensor && sensor->set_framesize(sensor, profile) == 0;
    if (switched) {
        sensor->set_quality(sensor, kJpegQuality);
        g_profile = profile;
    }
    esp_camera_fb_return(guard);

    if (!switched) ESP_LOGW(TAG, "OV3660 frame-size switch failed profile=%d", static_cast<int>(profile));
    return switched;
}

bool restore_motion_profile_before_return(camera_fb_t *held_snapshot) {
    sensor_t *sensor = esp_camera_sensor_get();
    const bool switched = sensor && sensor->set_framesize(sensor, kMotionFrameSize) == 0;
    if (switched) {
        sensor->set_quality(sensor, kJpegQuality);
        g_profile = kMotionFrameSize;
    } else {
        ESP_LOGW(TAG, "failed to restore motion profile while photo framebuffer held");
    }

    // Keep the photo checked out until after the sensor is back in QVGA. The
    // first capture after this return is therefore generated in motion mode.
    if (held_snapshot) esp_camera_fb_return(held_snapshot);
    return switched;
}

bool decode_motion(const uint8_t *jpeg, size_t len, uint8_t *out, size_t out_len) {
    if (!g_decoder || !jpeg || out_len < kMotionPixels) return false;
    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t *>(jpeg);
    io.inbuf_len = static_cast<int>(len);
    jpeg_dec_header_info_t info = {};
    if (jpeg_dec_parse_header(g_decoder, &io, &info) != JPEG_ERR_OK) return false;
    int decoded_len = 0;
    if (jpeg_dec_get_outbuf_len(g_decoder, &decoded_len) != JPEG_ERR_OK || decoded_len <= 0) return false;
    if (!ensure_rgb565(static_cast<size_t>(decoded_len))) return false;
    io.outbuf = g_rgb565;
    if (jpeg_dec_process(g_decoder, &io) != JPEG_ERR_OK) return false;

    const size_t pixels = std::min(kMotionPixels, static_cast<size_t>(decoded_len) / 2);
    for (size_t i = 0; i < pixels; ++i) {
        const uint16_t p = (static_cast<uint16_t>(g_rgb565[i * 2]) << 8) | g_rgb565[i * 2 + 1];
        const uint32_t r = ((p >> 11) & 0x1F) * 255 / 31;
        const uint32_t g = ((p >> 5) & 0x3F) * 255 / 63;
        const uint32_t b = (p & 0x1F) * 255 / 31;
        out[i] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
    }
    return pixels == kMotionPixels;
}
}  // namespace

bool begin() {
    if (g_initialized) return true;
    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) return false;

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
    config.frame_size = kMotionFrameSize;
    config.jpeg_quality = kJpegQuality;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.sccb_i2c_port = 0;

    const esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        return false;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        g_sensor_pid = sensor->id.PID;
        sensor->set_quality(sensor, kJpegQuality);
    }
    if (g_sensor_pid != 0x3660) {
        ESP_LOGW(TAG, "unexpected sensor PID=0x%04x", g_sensor_pid);
    }

    jpeg_dec_config_t decoder_config = DEFAULT_JPEG_DEC_CONFIG();
    decoder_config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    decoder_config.scale.width = kMotionWidth;
    decoder_config.scale.height = kMotionHeight;
    if (jpeg_dec_open(&decoder_config, &g_decoder) != JPEG_ERR_OK || !g_decoder) {
        ESP_LOGE(TAG, "JPEG decoder init failed");
        return false;
    }
    g_initialized = true;
    g_enabled = false;  // privacy-safe logical boot state; hardware stays warm.
    ESP_LOGI(TAG, "ready PID=0x%04x native JPEG q=%u 1FB PSRAM DMA OFF; logical camera OFF",
             g_sensor_pid, static_cast<unsigned>(kJpegQuality));
    return true;
}

bool set_enabled(bool enabled) {
    if (!g_initialized) return false;
    g_enabled = enabled;
    ESP_LOGI(TAG, "logical camera %s", enabled ? "ON" : "OFF");
    return true;
}

bool enabled() { return g_enabled; }
uint16_t sensor_pid() { return g_sensor_pid; }

bool capture_motion_luma(uint8_t *out, size_t out_len) {
    if (!g_initialized || !g_enabled || !out || out_len < kMotionPixels) return false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    bool ok = false;
    if (switch_profile_while_guarded(kMotionFrameSize)) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            const size_t clean_len = fb->format == PIXFORMAT_JPEG ? sanitized_jpeg_len(fb->buf, fb->len) : 0;
            ok = clean_len && decode_motion(fb->buf, clean_len, out, out_len);
            esp_camera_fb_return(fb);
        }
    }
    xSemaphoreGive(g_mutex);
    return ok;
}

bool capture_snapshot(Snapshot &snapshot) {
    snapshot = {};
    if (!g_initialized || !g_enabled) return false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1500)) != pdTRUE) return false;

    bool ok = false;
    if (switch_profile_while_guarded(kSnapshotFrameSize)) {
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
            if (!restore_motion_profile_before_return(fb)) ok = false;
        } else {
            ESP_LOGW(TAG, "snapshot framebuffer unavailable");
            // No frame is held, so use the guarded transition back to QVGA.
            switch_profile_while_guarded(kMotionFrameSize);
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

void release_snapshot(Snapshot &snapshot) {
    if (snapshot.jpeg) heap_caps_free(snapshot.jpeg);
    snapshot = {};
}
}  // namespace Newo2Camera
