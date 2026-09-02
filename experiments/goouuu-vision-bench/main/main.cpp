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

#include "web_ui.h"

namespace {

constexpr const char *TAG = "goouuu_vision";
constexpr const char *AP_SSID = "NEWO-CAM-TEST";
constexpr const char *AP_PASSWORD = "newovision";
constexpr int HISTORY_LEN = 100;
constexpr int MAX_FACES = 8;
constexpr float DEFAULT_SCORE_THRESHOLD = 0.30f;
constexpr float MIN_SCORE_THRESHOLD = 0.10f;
constexpr float MAX_SCORE_THRESHOLD = 0.90f;
constexpr int SOURCE_W = 640;
constexpr int SOURCE_H = 480;
constexpr int FAST_W = 320;
constexpr int FAST_H = 240;
constexpr uint8_t CAMERA_JPEG_QUALITY = 12;

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

enum class TestMode : uint8_t { VIEW = 0, FAST = 1, RANGE = 2, ACCURATE = 3, BENCH = 4 };

struct FaceBox {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    float score = 0.0f;
};

struct SharedJpeg {
    uint8_t *data = nullptr;
    size_t capacity = 0;
    size_t len = 0;
    uint16_t width = SOURCE_W;
    uint16_t height = SOURCE_H;
    uint32_t seq = 0;
};

struct JpegSnapshot {
    uint8_t *data = nullptr;
    size_t capacity = 0;
    size_t len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t seq = 0;
};

struct Metrics {
    uint32_t sensor_pid = 0;
    uint32_t source_width = SOURCE_W;
    uint32_t source_height = SOURCE_H;
    uint32_t source_jpeg_bytes = 0;
    uint32_t capture_ms = 0;
    uint32_t camera_frame_ms = 0;
    uint32_t stream_frame_ms = 0;
    uint32_t decode_ms = 0;
    uint32_t prep_ms = 0;
    uint32_t detect_ms = 0;
    uint32_t ai_total_ms = 0;
    uint32_t sweep_ms = 0;
    uint32_t ai_width = 0;
    uint32_t ai_height = 0;
    uint32_t range_tile = 0;
    uint32_t detector_passes = 0;
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
SharedJpeg g_latest;
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
        case TestMode::RANGE: return "range";
        case TestMode::ACCURATE: return "accurate";
        case TestMode::BENCH: return "bench";
    }
    return "unknown";
}

const char *model_name(TestMode mode) {
    return mode == TestMode::ACCURATE ? "ESPDet-224" : "MSR+MNP";
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

void free_snapshot(JpegSnapshot &s) {
    if (s.data) heap_caps_free(s.data);
    s = {};
}

void reset_history() {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.decode_ms = 0;
    g_metrics.prep_ms = 0;
    g_metrics.detect_ms = 0;
    g_metrics.ai_total_ms = 0;
    g_metrics.sweep_ms = 0;
    g_metrics.ai_width = 0;
    g_metrics.ai_height = 0;
    g_metrics.range_tile = 0;
    g_metrics.detector_passes = 0;
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

float box_iou(const FaceBox &a, const FaceBox &b) {
    const int ix1 = std::max(a.x1, b.x1);
    const int iy1 = std::max(a.y1, b.y1);
    const int ix2 = std::min(a.x2, b.x2);
    const int iy2 = std::min(a.y2, b.y2);
    const int iw = std::max(0, ix2 - ix1);
    const int ih = std::max(0, iy2 - iy1);
    const int inter = iw * ih;
    const int area_a = std::max(0, a.x2 - a.x1) * std::max(0, a.y2 - a.y1);
    const int area_b = std::max(0, b.x2 - b.x1) * std::max(0, b.y2 - b.y1);
    const int uni = area_a + area_b - inter;
    return uni > 0 ? static_cast<float>(inter) / static_cast<float>(uni) : 0.0f;
}

void add_box_dedup(FaceBox *boxes, int &face_count, const FaceBox &candidate) {
    for (int i = 0; i < face_count; ++i) {
        if (box_iou(boxes[i], candidate) >= 0.35f) {
            if (candidate.score > boxes[i].score) boxes[i] = candidate;
            return;
        }
    }
    if (face_count < MAX_FACES) boxes[face_count++] = candidate;
}

void append_scaled_results(const std::list<dl::detect::result_t> &results,
                           int input_w,
                           int input_h,
                           int source_w,
                           int source_h,
                           int offset_x,
                           int offset_y,
                           FaceBox *boxes,
                           int &face_count) {
    for (const auto &result : results) {
        if (result.box.size() < 4) continue;
        FaceBox b;
        b.x1 = offset_x + (result.box[0] * source_w) / input_w;
        b.y1 = offset_y + (result.box[1] * source_h) / input_h;
        b.x2 = offset_x + (result.box[2] * source_w) / input_w;
        b.y2 = offset_y + (result.box[3] * source_h) / input_h;
        b.score = result.score;
        b.x1 = std::clamp(b.x1, 0, SOURCE_W);
        b.y1 = std::clamp(b.y1, 0, SOURCE_H);
        b.x2 = std::clamp(b.x2, 0, SOURCE_W);
        b.y2 = std::clamp(b.y2, 0, SOURCE_H);
        if (b.x2 <= b.x1 || b.y2 <= b.y1) continue;
        add_box_dedup(boxes, face_count, b);
    }
}

void record_ai(uint32_t decode_ms,
               uint32_t prep_ms,
               uint32_t detect_ms,
               uint32_t ai_total_ms,
               uint32_t sweep_ms,
               uint32_t ai_w,
               uint32_t ai_h,
               uint32_t range_tile,
               const FaceBox *boxes,
               int face_count,
               bool detection_sample,
               bool update_boxes) {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.decode_ms = decode_ms;
    g_metrics.prep_ms = prep_ms;
    g_metrics.detect_ms = detect_ms;
    g_metrics.ai_total_ms = ai_total_ms;
    g_metrics.sweep_ms = sweep_ms;
    g_metrics.ai_width = ai_w;
    g_metrics.ai_height = ai_h;
    g_metrics.range_tile = range_tile;
    g_metrics.detector_passes = 1;

    if (detection_sample) {
        const bool hit = face_count > 0;
        const size_t idx = g_metrics.hist_index;
        g_metrics.hist_detect_ms[idx] = detect_ms;
        g_metrics.hist_hit[idx] = hit ? 1 : 0;
        g_metrics.hist_index = (idx + 1) % HISTORY_LEN;
        if (g_metrics.hist_count < HISTORY_LEN) ++g_metrics.hist_count;
        g_metrics.face_streak = hit ? g_metrics.face_streak + 1 : 0;
    }

    if (update_boxes) {
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
    }
    taskEXIT_CRITICAL(&g_metrics_mux);
}

void record_capture(uint32_t jpeg_bytes, uint32_t capture_ms, uint32_t frame_ms) {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.source_jpeg_bytes = jpeg_bytes;
    g_metrics.capture_ms = capture_ms;
    g_metrics.camera_frame_ms = frame_ms;
    taskEXIT_CRITICAL(&g_metrics_mux);
}

void record_stream_interval(uint32_t frame_ms) {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.stream_frame_ms = frame_ms;
    taskEXIT_CRITICAL(&g_metrics_mux);
}

bool snapshot_latest(JpegSnapshot &out, uint32_t last_seq) {
    if (!g_frame_mutex) return false;
    if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    if (!g_latest.data || g_latest.len == 0 || g_latest.seq == last_seq) {
        xSemaphoreGive(g_frame_mutex);
        return false;
    }
    if (!ensure_psram_buffer(out.data, out.capacity, g_latest.len)) {
        xSemaphoreGive(g_frame_mutex);
        return false;
    }
    memcpy(out.data, g_latest.data, g_latest.len);
    out.len = g_latest.len;
    out.width = g_latest.width;
    out.height = g_latest.height;
    out.seq = g_latest.seq;
    xSemaphoreGive(g_frame_mutex);
    return true;
}

bool decode_jpeg_rgb565(const JpegSnapshot &jpeg,
                        uint16_t target_w,
                        uint16_t target_h,
                        uint8_t *&decoded,
                        size_t &decoded_capacity,
                        uint16_t &out_w,
                        uint16_t &out_h,
                        uint32_t &decode_ms) {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    if (target_w && target_h && (target_w != jpeg.width || target_h != jpeg.height)) {
        config.scale.width = target_w;
        config.scale.height = target_h;
    }

    jpeg_dec_handle_t decoder = nullptr;
    const int64_t started = esp_timer_get_time();
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || !decoder) return false;

    jpeg_dec_io_t io = {};
    io.inbuf = jpeg.data;
    io.inbuf_len = static_cast<int>(jpeg.len);
    jpeg_dec_header_info_t info = {};
    if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK) {
        jpeg_dec_close(decoder);
        return false;
    }

    out_w = config.scale.width ? config.scale.width : info.width;
    out_h = config.scale.height ? config.scale.height : info.height;

    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(decoder, &out_len) != JPEG_ERR_OK || out_len <= 0) {
        jpeg_dec_close(decoder);
        return false;
    }
    if (!ensure_psram_buffer(decoded, decoded_capacity, static_cast<size_t>(out_len), true)) {
        jpeg_dec_close(decoder);
        return false;
    }

    io.outbuf = decoded;
    const jpeg_error_t result = jpeg_dec_process(decoder, &io);
    jpeg_dec_close(decoder);
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
                  int offset_x,
                  int offset_y,
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
    append_scaled_results(results, input_w, input_h, source_w, source_h, offset_x, offset_y, boxes, face_count);
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

    ESP_LOGI(TAG, "Initializing OV3660 native JPEG VGA, 2 framebuffers, PSRAM DMA OFF");
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
    for (;;) {
        if (xSemaphoreTake(g_camera_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        const int64_t started = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        const uint32_t capture_ms = static_cast<uint32_t>((esp_timer_get_time() - started + 500) / 1000);
        if (!fb) {
            xSemaphoreGive(g_camera_mutex);
            ESP_LOGW(TAG, "camera capture failed");
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        bool copied = false;
        if (fb->format == PIXFORMAT_JPEG && xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (ensure_psram_buffer(g_latest.data, g_latest.capacity, fb->len)) {
                memcpy(g_latest.data, fb->buf, fb->len);
                g_latest.len = fb->len;
                g_latest.width = fb->width;
                g_latest.height = fb->height;
                ++g_latest.seq;
                copied = true;
            }
            xSemaphoreGive(g_frame_mutex);
        }

        const uint32_t bytes = static_cast<uint32_t>(fb->len);
        esp_camera_fb_return(fb);
        xSemaphoreGive(g_camera_mutex);

        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        const uint32_t frame_ms = previous_ms ? now_ms - previous_ms : 0;
        previous_ms = now_ms;
        if (copied) record_capture(bytes, capture_ms, frame_ms);
        taskYIELD();
    }
}

void ai_task(void *) {
    JpegSnapshot jpeg;
    uint8_t *decoded = nullptr;
    size_t decoded_capacity = 0;
    uint8_t *tile = nullptr;
    size_t tile_capacity = 0;
    uint32_t last_seq = 0;
    TestMode previous_mode = TestMode::VIEW;
    int range_tile = 0;
    FaceBox sweep_boxes[MAX_FACES] = {};
    int sweep_face_count = 0;
    uint32_t sweep_compute_ms = 0;

    for (;;) {
        const TestMode mode = g_mode;
        if (mode == TestMode::VIEW) {
            vTaskDelay(pdMS_TO_TICKS(20));
            previous_mode = mode;
            continue;
        }
        if (mode != previous_mode) {
            range_tile = 0;
            sweep_face_count = 0;
            sweep_compute_ms = 0;
            memset(sweep_boxes, 0, sizeof(sweep_boxes));
            previous_mode = mode;
        }

        if (!snapshot_latest(jpeg, last_seq)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        last_seq = jpeg.seq;
        const int64_t ai_started = esp_timer_get_time();

        uint32_t decode_ms = 0;
        uint32_t prep_ms = 0;
        uint32_t detect_ms = 0;
        uint16_t decoded_w = 0;
        uint16_t decoded_h = 0;
        FaceBox boxes[MAX_FACES] = {};
        int face_count = 0;

        if (mode == TestMode::RANGE) {
            if (!decode_jpeg_rgb565(jpeg, jpeg.width, jpeg.height, decoded, decoded_capacity,
                                    decoded_w, decoded_h, decode_ms)) {
                ESP_LOGW(TAG, "RANGE jpeg decode failed");
                continue;
            }

            const int tile_w = decoded_w * 3 / 5;
            const int tile_h = decoded_h * 3 / 5;
            const int xs[2] = {0, static_cast<int>(decoded_w) - tile_w};
            const int ys[2] = {0, static_cast<int>(decoded_h) - tile_h};
            const int xi = range_tile & 1;
            const int yi = (range_tile >> 1) & 1;
            const int x0 = xs[xi];
            const int y0 = ys[yi];
            const size_t row_bytes = static_cast<size_t>(tile_w) * 2;
            const size_t tile_bytes = row_bytes * tile_h;
            if (!ensure_psram_buffer(tile, tile_capacity, tile_bytes, true)) {
                ESP_LOGE(TAG, "RANGE tile allocation failed");
                continue;
            }

            const int64_t prep_started = esp_timer_get_time();
            for (int y = 0; y < tile_h; ++y) {
                const uint8_t *src = decoded + (static_cast<size_t>(y0 + y) * decoded_w + x0) * 2;
                memcpy(tile + static_cast<size_t>(y) * row_bytes, src, row_bytes);
            }
            prep_ms = static_cast<uint32_t>((esp_timer_get_time() - prep_started + 500) / 1000);

            FaceBox tile_boxes[MAX_FACES] = {};
            int tile_face_count = 0;
            if (!run_detector(g_fast_detector, tile, tile_w, tile_h,
                              tile_boxes, tile_face_count, tile_w, tile_h, x0, y0, detect_ms)) {
                ESP_LOGW(TAG, "RANGE detector failed");
                continue;
            }
            for (int i = 0; i < tile_face_count; ++i) add_box_dedup(sweep_boxes, sweep_face_count, tile_boxes[i]);

            const uint32_t ai_total_ms = static_cast<uint32_t>((esp_timer_get_time() - ai_started + 500) / 1000);
            sweep_compute_ms += ai_total_ms;
            const bool sweep_complete = range_tile == 3;
            if (sweep_complete) {
                record_ai(decode_ms, prep_ms, detect_ms, ai_total_ms, sweep_compute_ms,
                          tile_w, tile_h, range_tile, sweep_boxes, sweep_face_count, true, true);
                sweep_face_count = 0;
                sweep_compute_ms = 0;
                memset(sweep_boxes, 0, sizeof(sweep_boxes));
            } else {
                record_ai(decode_ms, prep_ms, detect_ms, ai_total_ms, sweep_compute_ms,
                          tile_w, tile_h, range_tile, nullptr, 0, false, false);
            }
            range_tile = (range_tile + 1) & 3;
            continue;
        }

        if (!decode_jpeg_rgb565(jpeg, FAST_W, FAST_H, decoded, decoded_capacity,
                                decoded_w, decoded_h, decode_ms)) {
            ESP_LOGW(TAG, "JPEG decode failed in %s", mode_name(mode));
            continue;
        }

        HumanFaceDetect *detector = mode == TestMode::ACCURATE ? g_accurate_detector : g_fast_detector;
        if (!run_detector(detector, decoded, decoded_w, decoded_h,
                          boxes, face_count, jpeg.width, jpeg.height, 0, 0, detect_ms)) {
            ESP_LOGW(TAG, "detector failed in %s", mode_name(mode));
            continue;
        }
        const uint32_t ai_total_ms = static_cast<uint32_t>((esp_timer_get_time() - ai_started + 500) / 1000);
        record_ai(decode_ms, 0, detect_ms, ai_total_ms, 0,
                  decoded_w, decoded_h, 0, boxes, face_count, true, true);
    }

    free_snapshot(jpeg);
    if (decoded) heap_caps_free(decoded);
    if (tile) heap_caps_free(tile);
    vTaskDelete(nullptr);
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
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t mode_handler(httpd_req_t *req) {
    char value[16] = {};
    if (!get_query_value(req, "name", value, sizeof(value))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
    }
    if (!strcmp(value, "view")) set_mode(TestMode::VIEW);
    else if (!strcmp(value, "fast")) set_mode(TestMode::FAST);
    else if (!strcmp(value, "range")) set_mode(TestMode::RANGE);
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

    char json[4096];
    int n = snprintf(json, sizeof(json),
        "{\"mode\":\"%s\",\"model\":\"%s\",\"threshold\":%.2f"
        ",\"source_width\":%" PRIu32 ",\"source_height\":%" PRIu32
        ",\"sensor_pid\":%" PRIu32 ",\"jpeg_bytes\":%" PRIu32
        ",\"capture_ms\":%" PRIu32 ",\"camera_frame_ms\":%" PRIu32 ",\"stream_frame_ms\":%" PRIu32
        ",\"ai_width\":%" PRIu32 ",\"ai_height\":%" PRIu32
        ",\"decode_ms\":%" PRIu32 ",\"prep_ms\":%" PRIu32 ",\"detect_ms\":%" PRIu32
        ",\"ai_total_ms\":%" PRIu32 ",\"sweep_ms\":%" PRIu32 ",\"range_tile\":%" PRIu32
        ",\"detector_passes\":%" PRIu32 ",\"faces\":%d,\"face_streak\":%" PRIu32
        ",\"largest_face_w\":%" PRIu32 ",\"largest_face_h\":%" PRIu32
        ",\"detect_avg_ms\":%.2f,\"detect_p95_ms\":%" PRIu32
        ",\"samples\":%u,\"hits\":%" PRIu32 ",\"hit_rate\":%.2f"
        ",\"internal_free\":%" PRIu32 ",\"psram_free\":%" PRIu32 ",\"psram_largest\":%" PRIu32
        ",\"boxes\":[",
        mode_name(g_mode), model_name(g_mode), static_cast<double>(g_score_threshold),
        snapshot.source_width, snapshot.source_height, snapshot.sensor_pid, snapshot.source_jpeg_bytes,
        snapshot.capture_ms, snapshot.camera_frame_ms, snapshot.stream_frame_ms,
        snapshot.ai_width, snapshot.ai_height, snapshot.decode_ms, snapshot.prep_ms, snapshot.detect_ms,
        snapshot.ai_total_ms, snapshot.sweep_ms, snapshot.range_tile, snapshot.detector_passes,
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

    JpegSnapshot frame;
    uint32_t last_seq = 0;
    uint32_t previous_ms = 0;
    while (true) {
        if (g_mode == TestMode::BENCH) break;
        if (!snapshot_latest(frame, last_seq)) {
            vTaskDelay(pdMS_TO_TICKS(3));
            continue;
        }
        last_seq = frame.seq;

        char part[96];
        const int part_len = snprintf(part, sizeof(part), STREAM_PART, static_cast<unsigned>(frame.len));
        esp_err_t result = httpd_resp_send_chunk(req, STREAM_BOUNDARY, HTTPD_RESP_USE_STRLEN);
        if (result == ESP_OK) result = httpd_resp_send_chunk(req, part, part_len);
        if (result == ESP_OK) result = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(frame.data), frame.len);
        if (result != ESP_OK) break;

        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (previous_ms) record_stream_interval(now_ms - previous_ms);
        previous_ms = now_ms;
    }

    free_snapshot(frame);
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

    ESP_LOGI(TAG, "NEWO GOOUUU vision v4: native JPEG stream + decoupled AI");
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
    ESP_LOGI(TAG, "READY: native JPEG preview at http://192.168.4.1/ ; stream :81/stream");
}
