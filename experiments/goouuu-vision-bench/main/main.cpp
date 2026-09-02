#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dl_image_define.hpp"
#include "esp_camera.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "img_converters.h"
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

enum class TestMode : uint8_t { VIEW = 0, DETECT = 1, BENCH = 2 };

struct FaceBox {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    float score = 0.0f;
};

struct Metrics {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sensor_pid = 0;
    uint32_t capture_ms = 0;
    uint32_t decode_ms = 0;
    uint32_t detect_ms = 0;
    uint32_t pipeline_ms = 0;
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

volatile TestMode g_mode = TestMode::DETECT;
float g_score_threshold = DEFAULT_SCORE_THRESHOLD;
Metrics g_metrics;
portMUX_TYPE g_metrics_mux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t g_detector_mutex = nullptr;
HumanFaceDetect *g_detector = nullptr;
uint8_t *g_rgb = nullptr;
size_t g_rgb_capacity = 0;
httpd_handle_t g_httpd = nullptr;
httpd_handle_t g_stream_httpd = nullptr;

const char *mode_name(TestMode mode) {
    switch (mode) {
        case TestMode::VIEW: return "view";
        case TestMode::DETECT: return "detect";
        case TestMode::BENCH: return "bench";
    }
    return "unknown";
}

void reset_history() {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.capture_ms = 0;
    g_metrics.decode_ms = 0;
    g_metrics.detect_ms = 0;
    g_metrics.pipeline_ms = 0;
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
    if (!g_detector || !g_detector_mutex) return false;
    if (xSemaphoreTake(g_detector_mutex, portMAX_DELAY) != pdTRUE) return false;
    g_detector->set_score_thr(value, 0);
    g_detector->set_score_thr(value, 1);
    g_score_threshold = value;
    xSemaphoreGive(g_detector_mutex);
    reset_history();
    ESP_LOGI(TAG, "face detector score threshold=%.2f (both MSR and MNP stages)", static_cast<double>(value));
    return true;
}

void record_pipeline(uint32_t width,
                     uint32_t height,
                     uint32_t capture_ms,
                     uint32_t decode_ms,
                     uint32_t detect_ms,
                     uint32_t pipeline_ms,
                     const FaceBox *boxes,
                     int face_count,
                     bool detection_sample) {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.width = width;
    g_metrics.height = height;
    g_metrics.capture_ms = capture_ms;
    g_metrics.decode_ms = decode_ms;
    g_metrics.detect_ms = detect_ms;
    g_metrics.pipeline_ms = pipeline_ms;

    if (detection_sample) {
        const bool hit = face_count > 0;
        const size_t idx = g_metrics.hist_index;
        g_metrics.hist_detect_ms[idx] = detect_ms;
        g_metrics.hist_hit[idx] = hit ? 1 : 0;
        g_metrics.hist_index = (idx + 1) % HISTORY_LEN;
        if (g_metrics.hist_count < HISTORY_LEN) ++g_metrics.hist_count;
        g_metrics.face_streak = hit ? g_metrics.face_streak + 1 : 0;
    }

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

bool ensure_rgb_buffer(size_t required) {
    if (required <= g_rgb_capacity && g_rgb) return true;
    if (g_rgb) {
        heap_caps_free(g_rgb);
        g_rgb = nullptr;
        g_rgb_capacity = 0;
    }
    g_rgb = static_cast<uint8_t *>(heap_caps_malloc(required, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!g_rgb) {
        ESP_LOGE(TAG, "Unable to allocate %u bytes of PSRAM for RGB888", static_cast<unsigned>(required));
        return false;
    }
    g_rgb_capacity = required;
    return true;
}

bool run_face_detection(camera_fb_t *fb,
                        FaceBox *boxes,
                        int &face_count,
                        uint32_t &decode_ms,
                        uint32_t &detect_ms) {
    face_count = 0;
    decode_ms = 0;
    detect_ms = 0;
    if (xSemaphoreTake(g_detector_mutex, portMAX_DELAY) != pdTRUE) return false;

    const size_t required = fb->width * fb->height * 3;
    if (!ensure_rgb_buffer(required)) {
        xSemaphoreGive(g_detector_mutex);
        return false;
    }

    const int64_t decode_start = esp_timer_get_time();
    const bool converted = fmt2rgb888(fb->buf, fb->len, fb->format, g_rgb);
    decode_ms = static_cast<uint32_t>((esp_timer_get_time() - decode_start + 500) / 1000);
    if (!converted) {
        ESP_LOGE(TAG, "JPEG -> RGB888 conversion failed");
        xSemaphoreGive(g_detector_mutex);
        return false;
    }

    dl::image::img_t image = {
        .data = g_rgb,
        .width = static_cast<uint16_t>(fb->width),
        .height = static_cast<uint16_t>(fb->height),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
    };

    const int64_t detect_start = esp_timer_get_time();
    auto &results = g_detector->run(image);
    detect_ms = static_cast<uint32_t>((esp_timer_get_time() - detect_start + 500) / 1000);

    for (const auto &result : results) {
        if (face_count >= MAX_FACES) break;
        if (result.box.size() < 4) continue;
        FaceBox &b = boxes[face_count++];
        b.x1 = std::clamp(result.box[0], 0, static_cast<int>(fb->width));
        b.y1 = std::clamp(result.box[1], 0, static_cast<int>(fb->height));
        b.x2 = std::clamp(result.box[2], 0, static_cast<int>(fb->width));
        b.y2 = std::clamp(result.box[3], 0, static_cast<int>(fb->height));
        b.score = result.score;
    }

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
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 3;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.sccb_i2c_port = 0;

    ESP_LOGI(TAG, "Initializing GOOUUU camera: XCLK=%d SIOD=%d SIOC=%d", CAM_PIN_XCLK, CAM_PIN_SIOD, CAM_PIN_SIOC);
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) return err;

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        taskENTER_CRITICAL(&g_metrics_mux);
        g_metrics.sensor_pid = sensor->id.PID;
        g_metrics.width = 320;
        g_metrics.height = 240;
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
    ESP_LOGI(TAG, "Open http://192.168.4.1/ on the phone connected to that AP");
    return ESP_OK;
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
    if (!get_query_value(req, "name", value, sizeof(value))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
    TestMode next;
    if (!strcmp(value, "view")) next = TestMode::VIEW;
    else if (!strcmp(value, "detect")) next = TestMode::DETECT;
    else if (!strcmp(value, "bench")) next = TestMode::BENCH;
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mode");
    g_mode = next;
    reset_history();
    ESP_LOGI(TAG, "mode=%s", mode_name(next));
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t sensitivity_handler(httpd_req_t *req) {
    char value[16] = {};
    if (!get_query_value(req, "value", value, sizeof(value))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing value");
    char *end = nullptr;
    const float threshold = strtof(value, &end);
    if (end == value || *end != '\0' || threshold < MIN_SCORE_THRESHOLD || threshold > MAX_SCORE_THRESHOLD) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "threshold must be 0.10..0.90");
    }
    if (!set_detector_threshold(threshold)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "detector unavailable");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t resolution_handler(httpd_req_t *req) {
    char value[16] = {};
    if (!get_query_value(req, "name", value, sizeof(value))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
    framesize_t size;
    uint32_t w;
    uint32_t h;
    if (!strcmp(value, "qvga")) { size = FRAMESIZE_QVGA; w = 320; h = 240; }
    else if (!strcmp(value, "vga")) { size = FRAMESIZE_VGA; w = 640; h = 480; }
    else if (!strcmp(value, "svga")) { size = FRAMESIZE_SVGA; w = 800; h = 600; }
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad resolution");

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || sensor->set_framesize(sensor, size) != 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sensor rejected framesize");
    esp_camera_return_all();
    reset_history();
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.width = w;
    g_metrics.height = h;
    taskEXIT_CRITICAL(&g_metrics_mux);
    ESP_LOGI(TAG, "resolution=%s", value);
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t sensor_handler(httpd_req_t *req) {
    char var[20] = {};
    char val[8] = {};
    if (!get_query_value(req, "var", var, sizeof(var)) || !get_query_value(req, "val", val, sizeof(val))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing var/val");
    }
    const int value = atoi(val) ? 1 : 0;
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sensor unavailable");
    int rc = -1;
    if (!strcmp(var, "hmirror")) rc = sensor->set_hmirror(sensor, value);
    else if (!strcmp(var, "vflip")) rc = sensor->set_vflip(sensor, value);
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported sensor control");
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

    char json[4096];
    int n = snprintf(json, sizeof(json),
        "{\"mode\":\"%s\",\"threshold\":%.2f,\"width\":%" PRIu32 ",\"height\":%" PRIu32
        ",\"sensor_pid\":%" PRIu32 ",\"faces\":%d,\"face_streak\":%" PRIu32
        ",\"largest_face_w\":%" PRIu32 ",\"largest_face_h\":%" PRIu32
        ",\"capture_ms\":%" PRIu32 ",\"decode_ms\":%" PRIu32 ",\"detect_ms\":%" PRIu32
        ",\"pipeline_ms\":%" PRIu32 ",\"detect_avg_ms\":%.2f,\"detect_p95_ms\":%" PRIu32
        ",\"samples\":%u,\"hits\":%" PRIu32 ",\"hit_rate\":%.2f"
        ",\"heap_free\":%u,\"psram_free\":%u,\"boxes\":[",
        mode_name(g_mode), static_cast<double>(g_score_threshold), snapshot.width, snapshot.height,
        snapshot.sensor_pid, snapshot.face_count, snapshot.face_streak, snapshot.largest_face_w,
        snapshot.largest_face_h, snapshot.capture_ms, snapshot.decode_ms, snapshot.detect_ms,
        snapshot.pipeline_ms, avg, p95, static_cast<unsigned>(snapshot.hist_count), hits, hit_rate,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    for (int i = 0; i < snapshot.face_count && n > 0 && n < static_cast<int>(sizeof(json)); ++i) {
        const FaceBox &b = snapshot.boxes[i];
        n += snprintf(json + n, sizeof(json) - n,
            "%s{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d,\"score\":%.4f}",
            i ? "," : "", b.x1, b.y1, b.x2, b.y2, static_cast<double>(b.score));
    }
    if (n > 0 && n < static_cast<int>(sizeof(json) - 3)) {
        json[n++] = ']';
        json[n++] = '}';
        json[n] = '\0';
    } else {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "metrics overflow");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, n);
}

esp_err_t stream_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ESP_LOGI(TAG, "stream client connected");

    while (g_mode != TestMode::BENCH) {
        const int64_t cycle_start = esp_timer_get_time();
        const int64_t capture_start = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        const uint32_t capture_ms = static_cast<uint32_t>((esp_timer_get_time() - capture_start + 500) / 1000);
        if (!fb) {
            ESP_LOGE(TAG, "camera capture failed");
            break;
        }

        FaceBox boxes[MAX_FACES] = {};
        int face_count = 0;
        uint32_t decode_ms = 0;
        uint32_t detect_ms = 0;
        const bool do_detect = g_mode == TestMode::DETECT;
        if (do_detect) run_face_detection(fb, boxes, face_count, decode_ms, detect_ms);

        const uint32_t pipeline_ms = static_cast<uint32_t>((esp_timer_get_time() - cycle_start + 500) / 1000);
        record_pipeline(fb->width, fb->height, capture_ms, decode_ms, detect_ms, pipeline_ms,
                        boxes, face_count, do_detect);

        char part[96];
        const int part_len = snprintf(part, sizeof(part), STREAM_PART, static_cast<unsigned>(fb->len));
        esp_err_t result = httpd_resp_send_chunk(req, STREAM_BOUNDARY, HTTPD_RESP_USE_STRLEN);
        if (result == ESP_OK) result = httpd_resp_send_chunk(req, part, part_len);
        if (result == ESP_OK) result = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(fb->buf), fb->len);
        esp_camera_fb_return(fb);
        if (result != ESP_OK) break;
    }

    ESP_LOGI(TAG, "stream client disconnected");
    return ESP_OK;
}

void bench_task(void *) {
    while (true) {
        if (g_mode != TestMode::BENCH) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const int64_t cycle_start = esp_timer_get_time();
        const int64_t capture_start = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        const uint32_t capture_ms = static_cast<uint32_t>((esp_timer_get_time() - capture_start + 500) / 1000);
        if (!fb) {
            ESP_LOGE(TAG, "bench capture failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        FaceBox boxes[MAX_FACES] = {};
        int face_count = 0;
        uint32_t decode_ms = 0;
        uint32_t detect_ms = 0;
        run_face_detection(fb, boxes, face_count, decode_ms, detect_ms);
        const uint32_t pipeline_ms = static_cast<uint32_t>((esp_timer_get_time() - cycle_start + 500) / 1000);
        record_pipeline(fb->width, fb->height, capture_ms, decode_ms, detect_ms, pipeline_ms,
                        boxes, face_count, true);
        esp_camera_fb_return(fb);
        taskYIELD();
    }
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
    const httpd_uri_t resolution = {.uri = "/resolution", .method = HTTP_GET, .handler = resolution_handler, .user_ctx = nullptr};
    const httpd_uri_t sensor = {.uri = "/sensor", .method = HTTP_GET, .handler = sensor_handler, .user_ctx = nullptr};
    const httpd_uri_t reset = {.uri = "/reset", .method = HTTP_GET, .handler = reset_handler, .user_ctx = nullptr};
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &mode));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &sensitivity));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &metrics));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &resolution));
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

    ESP_LOGI(TAG, "NEWHO GOOUUU vision benchmark starting");
    ESP_LOGI(TAG, "PSRAM total=%u free=%u",
             static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    g_detector_mutex = xSemaphoreCreateMutex();
    if (!g_detector_mutex) {
        ESP_LOGE(TAG, "detector mutex allocation failed");
        abort();
    }

    ESP_LOGI(TAG, "Loading HumanFaceDetect MSR+MNP model...");
    g_detector = new HumanFaceDetect();
    if (!g_detector) {
        ESP_LOGE(TAG, "HumanFaceDetect allocation failed");
        abort();
    }
    if (!set_detector_threshold(DEFAULT_SCORE_THRESHOLD)) {
        ESP_LOGE(TAG, "Unable to configure detector threshold");
        abort();
    }

    ESP_ERROR_CHECK(init_camera());
    ESP_ERROR_CHECK(init_wifi_ap());
    ESP_ERROR_CHECK(start_web_servers());

    BaseType_t task_ok = xTaskCreatePinnedToCore(bench_task, "vision_bench", 8192, nullptr, 3, nullptr, 1);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "bench task creation failed");
        abort();
    }

    ESP_LOGI(TAG, "READY: connect to %s / %s then open http://192.168.4.1/", AP_SSID, AP_PASSWORD);
}
