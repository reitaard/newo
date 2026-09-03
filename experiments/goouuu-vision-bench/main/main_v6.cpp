#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>

#include "dl_image_define.hpp"
#include "esp_camera.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "nvs_flash.h"

#include "web_ui_v6.h"

namespace {

constexpr const char *TAG = "goouuu_vision";
constexpr const char *AP_SSID = "NEWO-CAM-TEST";
constexpr const char *AP_PASSWORD = "newovision";
constexpr const char *FW_NAME = "v6-stable";
constexpr int HISTORY_LEN = 100;
constexpr int MAX_FACES = 8;
constexpr int JPEG_POOL_SLOTS = 3;
constexpr float DEFAULT_SCORE_THRESHOLD = 0.30f;
constexpr float MIN_SCORE_THRESHOLD = 0.10f;
constexpr float MAX_SCORE_THRESHOLD = 0.90f;
constexpr int SOURCE_W = 640;
constexpr int SOURCE_H = 480;
constexpr int FAST_W = 320;
constexpr int FAST_H = 240;
constexpr uint8_t CAMERA_JPEG_QUALITY = 12;
constexpr size_t JPEG_EOI_SCAN_BYTES = 4096;

// GOOUUU ESP32-S3-CAM v1.5 / ESP32-S3-WROOM-1 N16R8.
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

const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

enum class TestMode : uint8_t { VIEW = 0, FAST = 1, ACCURATE = 2, BENCH = 3 };

struct FaceBox {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    float score = 0.0f;
};

struct JpegSlot {
    uint8_t *data = nullptr;
    size_t capacity = 0;
    size_t len = 0;
    uint16_t width = SOURCE_W;
    uint16_t height = SOURCE_H;
    uint32_t seq = 0;
    uint16_t refs = 0;
    bool writing = false;
};

struct JpegLease {
    const uint8_t *data = nullptr;
    size_t len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t seq = 0;
    int slot = -1;
};

struct DecoderContext {
    jpeg_dec_handle_t handle = nullptr;
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    const char *name = nullptr;
};

struct Metrics {
    uint32_t sensor_pid = 0;
    uint32_t source_width = SOURCE_W;
    uint32_t source_height = SOURCE_H;
    uint32_t source_jpeg_bytes = 0;
    uint32_t capture_ms = 0;
    uint32_t publish_copy_ms = 0;
    uint32_t camera_frame_ms = 0;
    uint32_t stream_frame_ms = 0;
    uint32_t pool_drops = 0;
    uint32_t jpeg_rejects = 0;
    uint32_t jpeg_trimmed = 0;
    uint32_t decode_ms = 0;
    uint32_t detect_ms = 0;
    uint32_t ai_total_ms = 0;
    uint32_t ai_width = 0;
    uint32_t ai_height = 0;
    uint32_t face_streak = 0;
    uint32_t largest_face_w = 0;
    uint32_t largest_face_h = 0;
    uint32_t hist_detect_ms[HISTORY_LEN] = {};
    uint8_t hist_hit[HISTORY_LEN] = {};
    size_t hist_index = 0;
    size_t hist_count = 0;
    FaceBox boxes[MAX_FACES] = {};
    int face_count = 0;
};

volatile TestMode g_mode = TestMode::FAST;
float g_score_threshold = DEFAULT_SCORE_THRESHOLD;
Metrics g_metrics;
std::array<JpegSlot, JPEG_POOL_SLOTS> g_jpeg_pool;
int g_latest_slot = -1;
uint32_t g_publish_seq = 0;
portMUX_TYPE g_metrics_mux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t g_frame_mutex = nullptr;
SemaphoreHandle_t g_camera_mutex = nullptr;
SemaphoreHandle_t g_detector_mutex = nullptr;
HumanFaceDetect *g_fast_detector = nullptr;
HumanFaceDetect *g_accurate_detector = nullptr;
httpd_handle_t g_httpd = nullptr;
httpd_handle_t g_stream_httpd = nullptr;

const char *mode_name(TestMode mode) {
    switch (mode) {
        case TestMode::VIEW: return "view";
        case TestMode::FAST: return "fast";
        case TestMode::ACCURATE: return "accurate";
        case TestMode::BENCH: return "bench";
    }
    return "unknown";
}

const char *model_name(TestMode mode) {
    return mode == TestMode::ACCURATE ? "ESPDet-224" : "MSR+MNP";
}

void ai_breathe() {
    // vision_ai runs above IDLE1. A real one-tick block lets CPU1's idle task run
    // and keeps the task watchdog meaningful instead of disabling it.
    vTaskDelay(1);
}

bool ensure_psram_buffer(uint8_t *&ptr, size_t &capacity, size_t required, bool aligned = false) {
    if (ptr && capacity >= required) return true;
    size_t next = (required + 65535u) & ~static_cast<size_t>(65535u);
    if (next < required) next = required;
    uint8_t *replacement = aligned
        ? static_cast<uint8_t *>(heap_caps_aligned_alloc(16, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
        : static_cast<uint8_t *>(heap_caps_malloc(next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!replacement) return false;
    if (ptr) heap_caps_free(ptr);
    ptr = replacement;
    capacity = next;
    return true;
}

void reset_history() {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.decode_ms = 0;
    g_metrics.detect_ms = 0;
    g_metrics.ai_total_ms = 0;
    g_metrics.ai_width = 0;
    g_metrics.ai_height = 0;
    g_metrics.face_streak = 0;
    g_metrics.largest_face_w = 0;
    g_metrics.largest_face_h = 0;
    g_metrics.hist_index = 0;
    g_metrics.hist_count = 0;
    g_metrics.face_count = 0;
    memset(g_metrics.hist_detect_ms, 0, sizeof(g_metrics.hist_detect_ms));
    memset(g_metrics.hist_hit, 0, sizeof(g_metrics.hist_hit));
    memset(g_metrics.boxes, 0, sizeof(g_metrics.boxes));
    taskEXIT_CRITICAL(&g_metrics_mux);
}

bool set_detector_threshold(float value) {
    value = std::clamp(value, MIN_SCORE_THRESHOLD, MAX_SCORE_THRESHOLD);
    if (!g_fast_detector || !g_accurate_detector || !g_detector_mutex) return false;
    if (xSemaphoreTake(g_detector_mutex, portMAX_DELAY) != pdTRUE) return false;
    g_fast_detector->set_score_thr(value, 0);
    g_fast_detector->set_score_thr(value, 1);
    g_accurate_detector->set_score_thr(value, 0);
    g_score_threshold = value;
    xSemaphoreGive(g_detector_mutex);
    reset_history();
    ESP_LOGI(TAG, "detector threshold=%.2f", static_cast<double>(value));
    return true;
}

void set_mode(TestMode next) {
    g_mode = next;
    reset_history();
    ESP_LOGI(TAG, "mode=%s source=VGA native JPEG model=%s", mode_name(next), model_name(next));
}

void append_results(const std::list<dl::detect::result_t> &results,
                    int input_w,
                    int input_h,
                    int source_w,
                    int source_h,
                    FaceBox *boxes,
                    int &face_count) {
    for (const auto &result : results) {
        if (result.box.size() < 4 || face_count >= MAX_FACES) continue;
        FaceBox b;
        b.x1 = (result.box[0] * source_w) / input_w;
        b.y1 = (result.box[1] * source_h) / input_h;
        b.x2 = (result.box[2] * source_w) / input_w;
        b.y2 = (result.box[3] * source_h) / input_h;
        b.score = result.score;
        b.x1 = std::clamp(b.x1, 0, SOURCE_W);
        b.y1 = std::clamp(b.y1, 0, SOURCE_H);
        b.x2 = std::clamp(b.x2, 0, SOURCE_W);
        b.y2 = std::clamp(b.y2, 0, SOURCE_H);
        if (b.x2 <= b.x1 || b.y2 <= b.y1) continue;
        boxes[face_count++] = b;
    }
}

void record_ai(uint32_t decode_ms,
               uint32_t detect_ms,
               uint32_t ai_total_ms,
               uint32_t ai_w,
               uint32_t ai_h,
               const FaceBox *boxes,
               int face_count) {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.decode_ms = decode_ms;
    g_metrics.detect_ms = detect_ms;
    g_metrics.ai_total_ms = ai_total_ms;
    g_metrics.ai_width = ai_w;
    g_metrics.ai_height = ai_h;

    const bool hit = face_count > 0;
    const size_t idx = g_metrics.hist_index;
    g_metrics.hist_detect_ms[idx] = detect_ms;
    g_metrics.hist_hit[idx] = hit ? 1 : 0;
    g_metrics.hist_index = (idx + 1) % HISTORY_LEN;
    if (g_metrics.hist_count < HISTORY_LEN) ++g_metrics.hist_count;
    g_metrics.face_streak = hit ? g_metrics.face_streak + 1 : 0;

    g_metrics.face_count = std::min(face_count, MAX_FACES);
    g_metrics.largest_face_w = 0;
    g_metrics.largest_face_h = 0;
    memset(g_metrics.boxes, 0, sizeof(g_metrics.boxes));
    for (int i = 0; i < g_metrics.face_count; ++i) {
        g_metrics.boxes[i] = boxes[i];
        const uint32_t w = boxes[i].x2 > boxes[i].x1 ? static_cast<uint32_t>(boxes[i].x2 - boxes[i].x1) : 0;
        const uint32_t h = boxes[i].y2 > boxes[i].y1 ? static_cast<uint32_t>(boxes[i].y2 - boxes[i].y1) : 0;
        if (w * h > g_metrics.largest_face_w * g_metrics.largest_face_h) {
            g_metrics.largest_face_w = w;
            g_metrics.largest_face_h = h;
        }
    }
    taskEXIT_CRITICAL(&g_metrics_mux);
}

void record_capture(uint32_t jpeg_bytes, uint32_t capture_ms, uint32_t publish_copy_ms, uint32_t frame_ms) {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.source_jpeg_bytes = jpeg_bytes;
    g_metrics.capture_ms = capture_ms;
    g_metrics.publish_copy_ms = publish_copy_ms;
    g_metrics.camera_frame_ms = frame_ms;
    taskEXIT_CRITICAL(&g_metrics_mux);
}

void record_pool_drop() {
    taskENTER_CRITICAL(&g_metrics_mux);
    ++g_metrics.pool_drops;
    taskEXIT_CRITICAL(&g_metrics_mux);
}

void record_jpeg_reject(bool trimmed) {
    taskENTER_CRITICAL(&g_metrics_mux);
    if (trimmed) ++g_metrics.jpeg_trimmed;
    else ++g_metrics.jpeg_rejects;
    taskEXIT_CRITICAL(&g_metrics_mux);
}

void record_stream_interval(uint32_t frame_ms) {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.stream_frame_ms = frame_ms;
    taskEXIT_CRITICAL(&g_metrics_mux);
}

size_t sanitized_jpeg_len(const uint8_t *data, size_t len, bool &trimmed) {
    trimmed = false;
    if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return 0;
    const size_t floor = len > JPEG_EOI_SCAN_BYTES ? len - JPEG_EOI_SCAN_BYTES : 2;
    for (size_t i = len - 1; i > floor; --i) {
        if (data[i - 1] == 0xFF && data[i] == 0xD9) {
            const size_t clean_len = i + 1;
            trimmed = clean_len != len;
            return clean_len;
        }
    }
    return 0;
}

int reserve_publish_slot() {
    if (!g_frame_mutex || xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    int chosen = -1;
    for (int i = 0; i < JPEG_POOL_SLOTS; ++i) {
        JpegSlot &slot = g_jpeg_pool[i];
        if (i != g_latest_slot && slot.refs == 0 && !slot.writing) {
            chosen = i;
            break;
        }
    }
    if (chosen < 0 && g_latest_slot >= 0) {
        JpegSlot &slot = g_jpeg_pool[g_latest_slot];
        if (slot.refs == 0 && !slot.writing) chosen = g_latest_slot;
    }
    if (chosen >= 0) g_jpeg_pool[chosen].writing = true;
    xSemaphoreGive(g_frame_mutex);
    return chosen;
}

void cancel_publish_slot(int slot_index) {
    if (slot_index < 0 || slot_index >= JPEG_POOL_SLOTS || !g_frame_mutex) return;
    if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_jpeg_pool[slot_index].writing = false;
        xSemaphoreGive(g_frame_mutex);
    }
}

void publish_slot(int slot_index, size_t len, uint16_t width, uint16_t height) {
    if (slot_index < 0 || slot_index >= JPEG_POOL_SLOTS || !g_frame_mutex) return;
    if (xSemaphoreTake(g_frame_mutex, portMAX_DELAY) == pdTRUE) {
        JpegSlot &slot = g_jpeg_pool[slot_index];
        slot.len = len;
        slot.width = width;
        slot.height = height;
        slot.seq = ++g_publish_seq;
        slot.writing = false;
        g_latest_slot = slot_index;
        xSemaphoreGive(g_frame_mutex);
    }
}

bool acquire_latest(JpegLease &lease, uint32_t last_seq) {
    lease = {};
    if (!g_frame_mutex || xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (g_latest_slot < 0) {
        xSemaphoreGive(g_frame_mutex);
        return false;
    }
    JpegSlot &slot = g_jpeg_pool[g_latest_slot];
    if (slot.writing || !slot.data || slot.len == 0 || slot.seq == last_seq) {
        xSemaphoreGive(g_frame_mutex);
        return false;
    }
    ++slot.refs;
    lease.data = slot.data;
    lease.len = slot.len;
    lease.width = slot.width;
    lease.height = slot.height;
    lease.seq = slot.seq;
    lease.slot = g_latest_slot;
    xSemaphoreGive(g_frame_mutex);
    return true;
}

void release_lease(JpegLease &lease) {
    if (lease.slot < 0 || lease.slot >= JPEG_POOL_SLOTS || !g_frame_mutex) {
        lease = {};
        return;
    }
    if (xSemaphoreTake(g_frame_mutex, portMAX_DELAY) == pdTRUE) {
        JpegSlot &slot = g_jpeg_pool[lease.slot];
        if (slot.refs > 0) --slot.refs;
        xSemaphoreGive(g_frame_mutex);
    }
    lease = {};
}

bool open_decoder(DecoderContext &ctx, const char *name, uint16_t scale_w, uint16_t scale_h) {
    ctx.config = DEFAULT_JPEG_DEC_CONFIG();
    ctx.config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    if (scale_w && scale_h) {
        ctx.config.scale.width = scale_w;
        ctx.config.scale.height = scale_h;
    }
    ctx.name = name;
    const jpeg_error_t err = jpeg_dec_open(&ctx.config, &ctx.handle);
    if (err != JPEG_ERR_OK || !ctx.handle) {
        ESP_LOGE(TAG, "jpeg decoder open failed name=%s err=%d", name, static_cast<int>(err));
        ctx.handle = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "jpeg decoder ready name=%s scale=%ux%u",
             name, static_cast<unsigned>(scale_w), static_cast<unsigned>(scale_h));
    return true;
}

void close_decoder(DecoderContext &ctx) {
    if (ctx.handle) jpeg_dec_close(ctx.handle);
    ctx.handle = nullptr;
}

bool decode_with_context(DecoderContext &ctx,
                         const JpegLease &jpeg,
                         uint8_t *&decoded,
                         size_t &decoded_capacity,
                         uint16_t &out_w,
                         uint16_t &out_h,
                         uint32_t &decode_ms) {
    if (!ctx.handle || !jpeg.data || jpeg.len == 0) return false;
    const int64_t started = esp_timer_get_time();

    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t *>(jpeg.data);
    io.inbuf_len = static_cast<int>(jpeg.len);
    jpeg_dec_header_info_t info = {};
    if (jpeg_dec_parse_header(ctx.handle, &io, &info) != JPEG_ERR_OK) return false;

    out_w = ctx.config.scale.width ? ctx.config.scale.width : info.width;
    out_h = ctx.config.scale.height ? ctx.config.scale.height : info.height;
    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(ctx.handle, &out_len) != JPEG_ERR_OK || out_len <= 0) return false;
    if (!ensure_psram_buffer(decoded, decoded_capacity, static_cast<size_t>(out_len), true)) return false;

    io.outbuf = decoded;
    const jpeg_error_t result = jpeg_dec_process(ctx.handle, &io);
    decode_ms = static_cast<uint32_t>((esp_timer_get_time() - started + 500) / 1000);
    return result == JPEG_ERR_OK;
}

bool run_detector(HumanFaceDetect *detector,
                  uint8_t *rgb565,
                  int input_w,
                  int input_h,
                  FaceBox *boxes,
                  int &face_count,
                  int source_w,
                  int source_h,
                  uint32_t &detect_ms) {
    if (!detector || !rgb565) return false;
    dl::image::img_t image = {
        .data = rgb565,
        .width = static_cast<uint16_t>(input_w),
        .height = static_cast<uint16_t>(input_h),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
    };
    if (xSemaphoreTake(g_detector_mutex, portMAX_DELAY) != pdTRUE) return false;
    const int64_t started = esp_timer_get_time();
    auto &results = detector->run(image);
    detect_ms = static_cast<uint32_t>((esp_timer_get_time() - started + 500) / 1000);
    append_results(results, input_w, input_h, source_w, source_h, boxes, face_count);
    xSemaphoreGive(g_detector_mutex);
    return true;
}

esp_err_t init_camera() {
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
    config.xclk_freq_hz = 20000000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = CAMERA_JPEG_QUALITY;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.sccb_i2c_port = 0;

    ESP_LOGI(TAG, "Initializing OV3660 native JPEG VGA q=%u, 2x128KiB FB, PSRAM DMA OFF",
             static_cast<unsigned>(CAMERA_JPEG_QUALITY));
    const esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) return err;

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        taskENTER_CRITICAL(&g_metrics_mux);
        g_metrics.sensor_pid = sensor->id.PID;
        taskEXIT_CRITICAL(&g_metrics_mux);
        ESP_LOGI(TAG, "Camera sensor PID=0x%04x", sensor->id.PID);
    }
    return ESP_OK;
}

esp_err_t init_wifi_ap() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    wifi_config_t ap = {};
    strlcpy(reinterpret_cast<char *>(ap.ap.ssid), AP_SSID, sizeof(ap.ap.ssid));
    strlcpy(reinterpret_cast<char *>(ap.ap.password), AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP ready: SSID=%s password=%s", AP_SSID, AP_PASSWORD);
    return ESP_OK;
}

void capture_task(void *) {
    uint32_t previous_ms = 0;
    uint32_t reject_log_count = 0;
    for (;;) {
        if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        const int64_t capture_started = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        const uint32_t capture_ms = static_cast<uint32_t>((esp_timer_get_time() - capture_started + 500) / 1000);
        if (!fb) {
            xSemaphoreGive(g_camera_mutex);
            ESP_LOGW(TAG, "camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        bool published = false;
        uint32_t publish_copy_ms = 0;
        uint32_t clean_bytes = 0;
        if (fb->format == PIXFORMAT_JPEG) {
            bool trimmed = false;
            const size_t clean_len = sanitized_jpeg_len(fb->buf, fb->len, trimmed);
            if (!clean_len) {
                record_jpeg_reject(false);
                if ((++reject_log_count % 16) == 1) {
                    ESP_LOGW(TAG, "rejecting malformed JPEG len=%u", static_cast<unsigned>(fb->len));
                }
            } else {
                if (trimmed) record_jpeg_reject(true);
                const int slot_index = reserve_publish_slot();
                if (slot_index >= 0) {
                    JpegSlot &slot = g_jpeg_pool[slot_index];
                    const int64_t copy_started = esp_timer_get_time();
                    if (ensure_psram_buffer(slot.data, slot.capacity, clean_len)) {
                        memcpy(slot.data, fb->buf, clean_len);
                        publish_copy_ms = static_cast<uint32_t>((esp_timer_get_time() - copy_started + 500) / 1000);
                        publish_slot(slot_index, clean_len, fb->width, fb->height);
                        clean_bytes = static_cast<uint32_t>(clean_len);
                        published = true;
                    } else {
                        cancel_publish_slot(slot_index);
                        ESP_LOGE(TAG, "JPEG pool allocation failed len=%u", static_cast<unsigned>(clean_len));
                    }
                } else {
                    record_pool_drop();
                }
            }
        }

        esp_camera_fb_return(fb);
        xSemaphoreGive(g_camera_mutex);

        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        const uint32_t frame_ms = previous_ms ? now_ms - previous_ms : 0;
        previous_ms = now_ms;
        if (published) record_capture(clean_bytes, capture_ms, publish_copy_ms, frame_ms);
        taskYIELD();
    }
}

void ai_task(void *) {
    DecoderContext fast_decoder;
    DecoderContext accurate_decoder;
    if (!open_decoder(fast_decoder, "fast-320x240", FAST_W, FAST_H) ||
        !open_decoder(accurate_decoder, "accurate-vga", 0, 0)) {
        ESP_LOGE(TAG, "AI decoder initialization failed");
        close_decoder(fast_decoder);
        close_decoder(accurate_decoder);
        vTaskDelete(nullptr);
        return;
    }

    uint8_t *decoded = nullptr;
    size_t decoded_capacity = 0;
    uint32_t last_seq = 0;

    for (;;) {
        const TestMode mode = g_mode;
        if (mode == TestMode::VIEW) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        JpegLease jpeg;
        if (!acquire_latest(jpeg, last_seq)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        last_seq = jpeg.seq;
        const int64_t ai_started = esp_timer_get_time();

        uint32_t decode_ms = 0;
        uint32_t detect_ms = 0;
        uint16_t decoded_w = 0;
        uint16_t decoded_h = 0;
        FaceBox boxes[MAX_FACES] = {};
        int face_count = 0;

        DecoderContext &decoder = mode == TestMode::ACCURATE ? accurate_decoder : fast_decoder;
        const bool decoded_ok = decode_with_context(decoder, jpeg, decoded, decoded_capacity,
                                                    decoded_w, decoded_h, decode_ms);
        const uint16_t source_w = jpeg.width;
        const uint16_t source_h = jpeg.height;
        release_lease(jpeg);
        if (!decoded_ok) {
            ESP_LOGW(TAG, "JPEG decode failed in %s", mode_name(mode));
            ai_breathe();
            continue;
        }

        HumanFaceDetect *detector = mode == TestMode::ACCURATE ? g_accurate_detector : g_fast_detector;
        if (!run_detector(detector, decoded, decoded_w, decoded_h,
                          boxes, face_count, source_w, source_h, detect_ms)) {
            ESP_LOGW(TAG, "detector failed in %s", mode_name(mode));
            ai_breathe();
            continue;
        }

        const uint32_t ai_total_ms = static_cast<uint32_t>((esp_timer_get_time() - ai_started + 500) / 1000);
        record_ai(decode_ms, detect_ms, ai_total_ms, decoded_w, decoded_h, boxes, face_count);
        ai_breathe();
    }
}

bool get_query_value(httpd_req_t *req, const char *key, char *out, size_t out_len) {
    const size_t len = httpd_req_get_url_query_len(req);
    if (!len || len > 256) return false;
    std::array<char, 257> query{};
    if (httpd_req_get_url_query_str(req, query.data(), std::min(query.size(), len + 1)) != ESP_OK) return false;
    return httpd_query_key_value(query.data(), key, out, out_len) == ESP_OK;
}

esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kIndexHtmlV6, HTTPD_RESP_USE_STRLEN);
}

esp_err_t mode_handler(httpd_req_t *req) {
    char value[16] = {};
    if (!get_query_value(req, "name", value, sizeof(value))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
    }
    if (!strcmp(value, "view")) set_mode(TestMode::VIEW);
    else if (!strcmp(value, "fast")) set_mode(TestMode::FAST);
    else if (!strcmp(value, "accurate")) set_mode(TestMode::ACCURATE);
    else if (!strcmp(value, "bench")) set_mode(TestMode::BENCH);
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mode");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t sensitivity_handler(httpd_req_t *req) {
    char value[16] = {};
    if (!get_query_value(req, "value", value, sizeof(value))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing value");
    }
    char *end = nullptr;
    const float threshold = strtof(value, &end);
    if (end == value || *end != '\0' || threshold < MIN_SCORE_THRESHOLD || threshold > MAX_SCORE_THRESHOLD) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "threshold must be 0.10..0.90");
    }
    if (!set_detector_threshold(threshold)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "detector unavailable");
    }
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t sensor_handler(httpd_req_t *req) {
    char var[20] = {};
    char val[8] = {};
    if (!get_query_value(req, "var", var, sizeof(var)) || !get_query_value(req, "val", val, sizeof(val))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing var/val");
    }
    const int value = atoi(val) ? 1 : 0;
    if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera busy");
    }
    sensor_t *sensor = esp_camera_sensor_get();
    int rc = -1;
    if (sensor) {
        if (!strcmp(var, "hmirror")) rc = sensor->set_hmirror(sensor, value);
        else if (!strcmp(var, "vflip")) rc = sensor->set_vflip(sensor, value);
    }
    xSemaphoreGive(g_camera_mutex);
    if (!sensor) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sensor unavailable");
    if (strcmp(var, "hmirror") && strcmp(var, "vflip")) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported sensor control");
    }
    if (rc != 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sensor control failed");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t reset_handler(httpd_req_t *req) {
    reset_history();
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t metrics_handler(httpd_req_t *req) {
    Metrics snapshot;
    taskENTER_CRITICAL(&g_metrics_mux);
    memcpy(&snapshot, &g_metrics, sizeof(snapshot));
    taskEXIT_CRITICAL(&g_metrics_mux);

    uint64_t sum = 0;
    uint32_t hits = 0;
    std::array<uint32_t, HISTORY_LEN> samples{};
    for (size_t i = 0; i < snapshot.hist_count; ++i) {
        sum += snapshot.hist_detect_ms[i];
        hits += snapshot.hist_hit[i];
        samples[i] = snapshot.hist_detect_ms[i];
    }
    std::sort(samples.begin(), samples.begin() + snapshot.hist_count);
    const uint32_t p95 = snapshot.hist_count ? samples[(snapshot.hist_count - 1) * 95 / 100] : 0;
    const double avg = snapshot.hist_count ? static_cast<double>(sum) / snapshot.hist_count : 0.0;
    const double hit_rate = snapshot.hist_count ? 100.0 * hits / snapshot.hist_count : 0.0;

    const uint32_t psram_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    const uint32_t psram_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    const uint32_t internal_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    char json[4352];
    int n = snprintf(json, sizeof(json),
        "{\"firmware\":\"%s\",\"mode\":\"%s\",\"model\":\"%s\",\"threshold\":%.2f"
        ",\"source_width\":%" PRIu32 ",\"source_height\":%" PRIu32
        ",\"sensor_pid\":%" PRIu32 ",\"jpeg_bytes\":%" PRIu32
        ",\"capture_ms\":%" PRIu32 ",\"publish_copy_ms\":%" PRIu32
        ",\"camera_frame_ms\":%" PRIu32 ",\"stream_frame_ms\":%" PRIu32
        ",\"pool_drops\":%" PRIu32 ",\"jpeg_rejects\":%" PRIu32 ",\"jpeg_trimmed\":%" PRIu32
        ",\"ai_width\":%" PRIu32 ",\"ai_height\":%" PRIu32
        ",\"decode_ms\":%" PRIu32 ",\"detect_ms\":%" PRIu32 ",\"ai_total_ms\":%" PRIu32
        ",\"faces\":%d,\"face_streak\":%" PRIu32
        ",\"largest_face_w\":%" PRIu32 ",\"largest_face_h\":%" PRIu32
        ",\"detect_avg_ms\":%.2f,\"detect_p95_ms\":%" PRIu32
        ",\"samples\":%u,\"hits\":%" PRIu32 ",\"hit_rate\":%.2f"
        ",\"internal_free\":%" PRIu32 ",\"psram_free\":%" PRIu32 ",\"psram_largest\":%" PRIu32
        ",\"boxes\":[",
        FW_NAME, mode_name(g_mode), model_name(g_mode), static_cast<double>(g_score_threshold),
        snapshot.source_width, snapshot.source_height, snapshot.sensor_pid, snapshot.source_jpeg_bytes,
        snapshot.capture_ms, snapshot.publish_copy_ms, snapshot.camera_frame_ms, snapshot.stream_frame_ms,
        snapshot.pool_drops, snapshot.jpeg_rejects, snapshot.jpeg_trimmed,
        snapshot.ai_width, snapshot.ai_height, snapshot.decode_ms, snapshot.detect_ms, snapshot.ai_total_ms,
        snapshot.face_count, snapshot.face_streak, snapshot.largest_face_w, snapshot.largest_face_h,
        avg, p95, static_cast<unsigned>(snapshot.hist_count), hits, hit_rate,
        internal_free, psram_free, psram_largest);

    for (int i = 0; i < snapshot.face_count && n > 0 && n < static_cast<int>(sizeof(json)); ++i) {
        const FaceBox &b = snapshot.boxes[i];
        n += snprintf(json + n, sizeof(json) - n,
            "%s{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d,\"score\":%.4f}",
            i ? "," : "", b.x1, b.y1, b.x2, b.y2, static_cast<double>(b.score));
    }
    if (n <= 0 || n >= static_cast<int>(sizeof(json) - 3)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "metrics overflow");
    }
    json[n++] = ']';
    json[n++] = '}';
    json[n] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, n);
}

esp_err_t stream_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ESP_LOGI(TAG, "native JPEG stream client connected");

    JpegLease frame;
    uint32_t last_seq = 0;
    uint32_t previous_ms = 0;
    while (true) {
        if (g_mode == TestMode::BENCH) break;
        if (!acquire_latest(frame, last_seq)) {
            vTaskDelay(pdMS_TO_TICKS(3));
            continue;
        }
        last_seq = frame.seq;

        char part[96];
        const int part_len = snprintf(part, sizeof(part), STREAM_PART, static_cast<unsigned>(frame.len));
        esp_err_t result = httpd_resp_send_chunk(req, STREAM_BOUNDARY, HTTPD_RESP_USE_STRLEN);
        if (result == ESP_OK) result = httpd_resp_send_chunk(req, part, part_len);
        if (result == ESP_OK) result = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(frame.data), frame.len);
        release_lease(frame);
        if (result != ESP_OK) break;

        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (previous_ms) record_stream_interval(now_ms - previous_ms);
        previous_ms = now_ms;
    }

    release_lease(frame);
    ESP_LOGI(TAG, "native JPEG stream client disconnected");
    return ESP_OK;
}

esp_err_t start_web_servers() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&g_httpd, &config));

    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = nullptr};
    const httpd_uri_t mode = {.uri = "/mode", .method = HTTP_GET, .handler = mode_handler, .user_ctx = nullptr};
    const httpd_uri_t sensitivity = {.uri = "/sensitivity", .method = HTTP_GET, .handler = sensitivity_handler, .user_ctx = nullptr};
    const httpd_uri_t metrics = {.uri = "/metrics", .method = HTTP_GET, .handler = metrics_handler, .user_ctx = nullptr};
    const httpd_uri_t sensor = {.uri = "/sensor", .method = HTTP_GET, .handler = sensor_handler, .user_ctx = nullptr};
    const httpd_uri_t reset = {.uri = "/reset", .method = HTTP_GET, .handler = reset_handler, .user_ctx = nullptr};
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &mode));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &sensitivity));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &metrics));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &sensor));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &reset));

    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port = 81;
    stream_config.ctrl_port = 32769;
    stream_config.lru_purge_enable = true;
    stream_config.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&g_stream_httpd, &stream_config));
    const httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = nullptr};
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_stream_httpd, &stream));
    return ESP_OK;
}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    ESP_LOGI(TAG, "NEWO GOOUUU vision v6 stable: native JPEG + FAST/ACCURATE");
    ESP_LOGI(TAG, "PSRAM total=%u free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));

    g_frame_mutex = xSemaphoreCreateMutex();
    g_camera_mutex = xSemaphoreCreateMutex();
    g_detector_mutex = xSemaphoreCreateMutex();
    if (!g_frame_mutex || !g_camera_mutex || !g_detector_mutex) {
        ESP_LOGE(TAG, "mutex allocation failed");
        abort();
    }

    g_fast_detector = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, true);
    g_accurate_detector = new HumanFaceDetect(HumanFaceDetect::ESPDET_PICO_224_224_FACE, true);
    if (!g_fast_detector || !g_accurate_detector) {
        ESP_LOGE(TAG, "detector allocation failed");
        abort();
    }
    if (!set_detector_threshold(DEFAULT_SCORE_THRESHOLD)) {
        ESP_LOGE(TAG, "detector threshold setup failed");
        abort();
    }

    ESP_ERROR_CHECK(init_camera());
    ESP_ERROR_CHECK(init_wifi_ap());

    if (xTaskCreatePinnedToCore(capture_task, "vision_capture", 6144, nullptr, 4, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "capture task creation failed");
        abort();
    }
    if (xTaskCreatePinnedToCore(ai_task, "vision_ai", 12288, nullptr, 3, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "AI task creation failed");
        abort();
    }

    ESP_ERROR_CHECK(start_web_servers());
    ESP_LOGI(TAG, "READY: v6 stable preview at http://192.168.4.1/ ; stream :81/stream");
}
