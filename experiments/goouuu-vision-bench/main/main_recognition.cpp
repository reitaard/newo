#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <vector>

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
#include "human_face_recognition.hpp"
#include "nvs_flash.h"

#include "web_ui_recognition.h"

namespace {

constexpr const char *TAG = "goouuu_recognition";
constexpr const char *AP_SSID = "NEWO-CAM-TEST";
constexpr const char *AP_PASSWORD = "newovision";
constexpr const char *FW_NAME = "recognition-v1";
constexpr int SOURCE_W = 640;
constexpr int SOURCE_H = 480;
constexpr int MAX_FACES = 8;
constexpr int JPEG_POOL_SLOTS = 3;
constexpr int MAX_ENROLL_SAMPLES = 5;
constexpr float DETECTOR_THRESHOLD = 0.30f;
constexpr float DEFAULT_RECOGNITION_THRESHOLD = 0.50f;
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

enum class ExpectedMode : uint8_t { OFF = 0, ME = 1, OTHER = 2 };

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
};

struct EnrollmentSample {
    float *feat = nullptr;
    int len = 0;
};

struct Metrics {
    uint32_t sensor_pid = 0;
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
    uint32_t recognition_ms = 0;
    uint32_t ai_total_ms = 0;
    uint32_t largest_face_w = 0;
    uint32_t largest_face_h = 0;
    uint32_t test_frames = 0;
    uint32_t face_frames = 0;
    uint32_t recognition_attempts = 0;
    uint32_t correct_classifications = 0;
    uint32_t landmark_failures = 0;
    int enrollment_count = 0;
    int face_count = 0;
    float recognition_threshold = DEFAULT_RECOGNITION_THRESHOLD;
    float similarity_max = 0.0f;
    float similarity_avg = 0.0f;
    char identity[20] = "NOT ENROLLED";
    FaceBox boxes[MAX_FACES] = {};
};

std::array<JpegSlot, JPEG_POOL_SLOTS> g_jpeg_pool;
std::array<EnrollmentSample, MAX_ENROLL_SAMPLES> g_enrollment;
int g_latest_slot = -1;
uint32_t g_publish_seq = 0;
int g_enrollment_count = 0;
volatile bool g_enroll_requested = false;
volatile ExpectedMode g_expected = ExpectedMode::OFF;
float g_recognition_threshold = DEFAULT_RECOGNITION_THRESHOLD;

Metrics g_metrics;
portMUX_TYPE g_metrics_mux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t g_frame_mutex = nullptr;
SemaphoreHandle_t g_camera_mutex = nullptr;
SemaphoreHandle_t g_rec_mutex = nullptr;
HumanFaceDetect *g_detector = nullptr;
HumanFaceFeat *g_feat = nullptr;
httpd_handle_t g_httpd = nullptr;
httpd_handle_t g_stream_httpd = nullptr;

const char *expected_name(ExpectedMode mode) {
    switch (mode) {
        case ExpectedMode::ME: return "me";
        case ExpectedMode::OTHER: return "other";
        default: return "off";
    }
}

void ai_breathe() {
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

void set_identity(const char *identity, float sim_max, float sim_avg, uint32_t rec_ms) {
    taskENTER_CRITICAL(&g_metrics_mux);
    strlcpy(g_metrics.identity, identity, sizeof(g_metrics.identity));
    g_metrics.similarity_max = sim_max;
    g_metrics.similarity_avg = sim_avg;
    g_metrics.recognition_ms = rec_ms;
    taskEXIT_CRITICAL(&g_metrics_mux);
}

void reset_test_counters() {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.test_frames = 0;
    g_metrics.face_frames = 0;
    g_metrics.recognition_attempts = 0;
    g_metrics.correct_classifications = 0;
    g_metrics.landmark_failures = 0;
    taskEXIT_CRITICAL(&g_metrics_mux);
}

void clear_enrollment() {
    if (!g_rec_mutex || xSemaphoreTake(g_rec_mutex, portMAX_DELAY) != pdTRUE) return;
    for (auto &sample : g_enrollment) {
        if (sample.feat) heap_caps_free(sample.feat);
        sample = {};
    }
    g_enrollment_count = 0;
    g_enroll_requested = false;
    xSemaphoreGive(g_rec_mutex);

    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.enrollment_count = 0;
    g_metrics.similarity_max = 0.0f;
    g_metrics.similarity_avg = 0.0f;
    g_metrics.recognition_ms = 0;
    strlcpy(g_metrics.identity, "NOT ENROLLED", sizeof(g_metrics.identity));
    taskEXIT_CRITICAL(&g_metrics_mux);
    reset_test_counters();
    ESP_LOGI(TAG, "enrollment cleared");
}

bool store_embedding(dl::TensorBase *feat) {
    if (!feat || feat->dtype != dl::DATA_TYPE_FLOAT || feat->size <= 0) return false;
    if (xSemaphoreTake(g_rec_mutex, portMAX_DELAY) != pdTRUE) return false;
    bool ok = false;
    if (g_enrollment_count < MAX_ENROLL_SAMPLES) {
        const int len = feat->size;
        float *copy = static_cast<float *>(heap_caps_malloc(static_cast<size_t>(len) * sizeof(float), MALLOC_CAP_SPIRAM));
        if (copy) {
            memcpy(copy, feat->data, static_cast<size_t>(len) * sizeof(float));
            g_enrollment[g_enrollment_count].feat = copy;
            g_enrollment[g_enrollment_count].len = len;
            ++g_enrollment_count;
            ok = true;
        }
    }
    const int count = g_enrollment_count;
    xSemaphoreGive(g_rec_mutex);
    if (ok) {
        taskENTER_CRITICAL(&g_metrics_mux);
        g_metrics.enrollment_count = count;
        taskEXIT_CRITICAL(&g_metrics_mux);
        ESP_LOGI(TAG, "enrolled MFN sample %d/%d feat_len=%d psram_free=%u",
                 count, MAX_ENROLL_SAMPLES, feat->size,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
    return ok;
}

bool compare_embedding(dl::TensorBase *feat, float &sim_max, float &sim_avg, int &sample_count) {
    sim_max = -1.0f;
    sim_avg = 0.0f;
    sample_count = 0;
    if (!feat || feat->dtype != dl::DATA_TYPE_FLOAT || feat->size <= 0) return false;
    if (xSemaphoreTake(g_rec_mutex, portMAX_DELAY) != pdTRUE) return false;
    const float *current = static_cast<const float *>(feat->data);
    for (int s = 0; s < g_enrollment_count; ++s) {
        const EnrollmentSample &sample = g_enrollment[s];
        if (!sample.feat || sample.len != feat->size) continue;
        float dot = 0.0f;
        for (int i = 0; i < feat->size; ++i) dot += current[i] * sample.feat[i];
        sim_max = std::max(sim_max, dot);
        sim_avg += dot;
        ++sample_count;
    }
    xSemaphoreGive(g_rec_mutex);
    if (!sample_count) return false;
    sim_avg /= static_cast<float>(sample_count);
    return true;
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
    if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
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
    if (slot_index < 0 || slot_index >= JPEG_POOL_SLOTS) return;
    if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_jpeg_pool[slot_index].writing = false;
        xSemaphoreGive(g_frame_mutex);
    }
}

void publish_slot(int slot_index, size_t len, uint16_t width, uint16_t height) {
    if (xSemaphoreTake(g_frame_mutex, portMAX_DELAY) != pdTRUE) return;
    JpegSlot &slot = g_jpeg_pool[slot_index];
    slot.len = len;
    slot.width = width;
    slot.height = height;
    slot.seq = ++g_publish_seq;
    slot.writing = false;
    g_latest_slot = slot_index;
    xSemaphoreGive(g_frame_mutex);
}

bool acquire_latest(JpegLease &lease, uint32_t last_seq) {
    lease = {};
    if (xSemaphoreTake(g_frame_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (g_latest_slot < 0) {
        xSemaphoreGive(g_frame_mutex);
        return false;
    }
    JpegSlot &slot = g_jpeg_pool[g_latest_slot];
    if (slot.writing || !slot.data || !slot.len || slot.seq == last_seq) {
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
    if (lease.slot < 0 || lease.slot >= JPEG_POOL_SLOTS) {
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

bool open_decoder(DecoderContext &ctx) {
    ctx.config = DEFAULT_JPEG_DEC_CONFIG();
    ctx.config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    const jpeg_error_t err = jpeg_dec_open(&ctx.config, &ctx.handle);
    if (err != JPEG_ERR_OK || !ctx.handle) {
        ESP_LOGE(TAG, "jpeg decoder open failed err=%d", static_cast<int>(err));
        return false;
    }
    ESP_LOGI(TAG, "VGA RGB565BE jpeg decoder ready");
    return true;
}

bool decode_jpeg(DecoderContext &ctx, const JpegLease &jpeg, uint8_t *&decoded, size_t &capacity,
                 uint16_t &out_w, uint16_t &out_h, uint32_t &decode_ms) {
    const int64_t started = esp_timer_get_time();
    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t *>(jpeg.data);
    io.inbuf_len = static_cast<int>(jpeg.len);
    jpeg_dec_header_info_t info = {};
    if (jpeg_dec_parse_header(ctx.handle, &io, &info) != JPEG_ERR_OK) return false;
    out_w = info.width;
    out_h = info.height;
    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(ctx.handle, &out_len) != JPEG_ERR_OK || out_len <= 0) return false;
    if (!ensure_psram_buffer(decoded, capacity, static_cast<size_t>(out_len), true)) return false;
    io.outbuf = decoded;
    const jpeg_error_t err = jpeg_dec_process(ctx.handle, &io);
    decode_ms = static_cast<uint32_t>((esp_timer_get_time() - started + 500) / 1000);
    return err == JPEG_ERR_OK;
}

const dl::detect::result_t *largest_face(const std::list<dl::detect::result_t> &results) {
    if (results.empty()) return nullptr;
    return &*std::max_element(results.begin(), results.end(), [](const auto &a, const auto &b) {
        return a.box_area() < b.box_area();
    });
}

void record_vision(uint32_t decode_ms, uint32_t detect_ms, uint32_t ai_total_ms,
                   const std::list<dl::detect::result_t> &results) {
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.decode_ms = decode_ms;
    g_metrics.detect_ms = detect_ms;
    g_metrics.ai_total_ms = ai_total_ms;
    g_metrics.face_count = 0;
    g_metrics.largest_face_w = 0;
    g_metrics.largest_face_h = 0;
    memset(g_metrics.boxes, 0, sizeof(g_metrics.boxes));
    for (const auto &r : results) {
        if (r.box.size() < 4 || g_metrics.face_count >= MAX_FACES) continue;
        FaceBox &b = g_metrics.boxes[g_metrics.face_count++];
        b.x1 = std::clamp(r.box[0], 0, SOURCE_W);
        b.y1 = std::clamp(r.box[1], 0, SOURCE_H);
        b.x2 = std::clamp(r.box[2], 0, SOURCE_W);
        b.y2 = std::clamp(r.box[3], 0, SOURCE_H);
        b.score = r.score;
        const uint32_t w = b.x2 > b.x1 ? static_cast<uint32_t>(b.x2 - b.x1) : 0;
        const uint32_t h = b.y2 > b.y1 ? static_cast<uint32_t>(b.y2 - b.y1) : 0;
        if (w * h > g_metrics.largest_face_w * g_metrics.largest_face_h) {
            g_metrics.largest_face_w = w;
            g_metrics.largest_face_h = h;
        }
    }
    taskEXIT_CRITICAL(&g_metrics_mux);
}

void record_test_frame(bool face_available, bool rec_attempted, bool correct) {
    if (g_expected == ExpectedMode::OFF) return;
    taskENTER_CRITICAL(&g_metrics_mux);
    ++g_metrics.test_frames;
    if (face_available) ++g_metrics.face_frames;
    if (rec_attempted) ++g_metrics.recognition_attempts;
    if (correct) ++g_metrics.correct_classifications;
    taskEXIT_CRITICAL(&g_metrics_mux);
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
        const int64_t started = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        const uint32_t capture_ms = static_cast<uint32_t>((esp_timer_get_time() - started + 500) / 1000);
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
                taskENTER_CRITICAL(&g_metrics_mux);
                ++g_metrics.jpeg_rejects;
                taskEXIT_CRITICAL(&g_metrics_mux);
                if ((++reject_log_count % 16) == 1) ESP_LOGW(TAG, "rejecting malformed JPEG len=%u", static_cast<unsigned>(fb->len));
            } else {
                if (trimmed) {
                    taskENTER_CRITICAL(&g_metrics_mux);
                    ++g_metrics.jpeg_trimmed;
                    taskEXIT_CRITICAL(&g_metrics_mux);
                }
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
                        ESP_LOGE(TAG, "JPEG pool allocation failed");
                    }
                } else {
                    taskENTER_CRITICAL(&g_metrics_mux);
                    ++g_metrics.pool_drops;
                    taskEXIT_CRITICAL(&g_metrics_mux);
                }
            }
        }
        esp_camera_fb_return(fb);
        xSemaphoreGive(g_camera_mutex);

        const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        const uint32_t frame_ms = previous_ms ? now_ms - previous_ms : 0;
        previous_ms = now_ms;
        if (published) {
            taskENTER_CRITICAL(&g_metrics_mux);
            g_metrics.source_jpeg_bytes = clean_bytes;
            g_metrics.capture_ms = capture_ms;
            g_metrics.publish_copy_ms = publish_copy_ms;
            g_metrics.camera_frame_ms = frame_ms;
            taskEXIT_CRITICAL(&g_metrics_mux);
        }
        taskYIELD();
    }
}

void ai_task(void *) {
    DecoderContext decoder;
    if (!open_decoder(decoder)) {
        ESP_LOGE(TAG, "decoder initialization failed");
        vTaskDelete(nullptr);
        return;
    }
    uint8_t *decoded = nullptr;
    size_t decoded_capacity = 0;
    uint32_t last_seq = 0;

    for (;;) {
        JpegLease jpeg;
        if (!acquire_latest(jpeg, last_seq)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        last_seq = jpeg.seq;
        const int64_t ai_started = esp_timer_get_time();
        uint16_t w = 0, h = 0;
        uint32_t decode_ms = 0;
        const bool decoded_ok = decode_jpeg(decoder, jpeg, decoded, decoded_capacity, w, h, decode_ms);
        release_lease(jpeg);
        if (!decoded_ok || w != SOURCE_W || h != SOURCE_H) {
            ESP_LOGW(TAG, "JPEG decode failed or unexpected size %ux%u", static_cast<unsigned>(w), static_cast<unsigned>(h));
            ai_breathe();
            continue;
        }

        dl::image::img_t image = {
            .data = decoded,
            .width = static_cast<uint16_t>(w),
            .height = static_cast<uint16_t>(h),
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
        };
        const int64_t detect_started = esp_timer_get_time();
        auto &results = g_detector->run(image);
        const uint32_t detect_ms = static_cast<uint32_t>((esp_timer_get_time() - detect_started + 500) / 1000);
        const dl::detect::result_t *best = largest_face(results);
        const bool face_available = best != nullptr;

        bool rec_attempted = false;
        bool correct = false;
        uint32_t recognition_ms = 0;
        float sim_max = 0.0f, sim_avg = 0.0f;
        const bool enrolled = g_enrollment_count > 0;
        const bool want_enroll = g_enroll_requested;

        if (best && best->keypoint.size() == 10 && (want_enroll || enrolled)) {
            const int64_t rec_started = esp_timer_get_time();
            dl::TensorBase *feat = g_feat->run(image, best->keypoint);
            recognition_ms = static_cast<uint32_t>((esp_timer_get_time() - rec_started + 500) / 1000);
            if (feat) {
                if (want_enroll) {
                    if (results.size() == 1 && store_embedding(feat)) {
                        g_enroll_requested = false;
                    } else if (results.size() != 1) {
                        ESP_LOGW(TAG, "enroll waiting for exactly one face; faces=%u", static_cast<unsigned>(results.size()));
                    }
                }
                int sample_count = 0;
                if (compare_embedding(feat, sim_max, sim_avg, sample_count)) {
                    rec_attempted = true;
                    const bool recognized = sim_max >= g_recognition_threshold;
                    set_identity(recognized ? "ME" : "UNKNOWN", sim_max, sim_avg, recognition_ms);
                    if (g_expected == ExpectedMode::ME) correct = recognized;
                    else if (g_expected == ExpectedMode::OTHER) correct = !recognized;
                }
            }
        } else if (best && best->keypoint.size() != 10) {
            taskENTER_CRITICAL(&g_metrics_mux);
            ++g_metrics.landmark_failures;
            taskEXIT_CRITICAL(&g_metrics_mux);
            set_identity("NO LANDMARKS", 0.0f, 0.0f, 0);
        } else if (!best) {
            set_identity(enrolled ? "NO FACE" : "NOT ENROLLED", 0.0f, 0.0f, 0);
        } else if (!enrolled && !want_enroll) {
            set_identity("NOT ENROLLED", 0.0f, 0.0f, 0);
        }

        const uint32_t ai_total_ms = static_cast<uint32_t>((esp_timer_get_time() - ai_started + 500) / 1000);
        record_vision(decode_ms, detect_ms, ai_total_ms, results);
        record_test_frame(face_available, rec_attempted, correct);
        ai_breathe();
    }
}

bool get_query_value(httpd_req_t *req, const char *key, char *out, size_t out_len) {
    const size_t len = httpd_req_get_url_query_len(req);
    if (!len || len > 128) return false;
    std::array<char, 129> query{};
    if (httpd_req_get_url_query_str(req, query.data(), std::min(query.size(), len + 1)) != ESP_OK) return false;
    return httpd_query_key_value(query.data(), key, out, out_len) == ESP_OK;
}

esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kRecognitionHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t enroll_handler(httpd_req_t *req) {
    if (g_enrollment_count >= MAX_ENROLL_SAMPLES) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enrollment full");
    g_enroll_requested = true;
    ESP_LOGI(TAG, "enroll requested: hold one face steady");
    return httpd_resp_sendstr(req, "queued");
}

esp_err_t clear_handler(httpd_req_t *req) {
    clear_enrollment();
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t expected_handler(httpd_req_t *req) {
    char value[12] = {};
    if (!get_query_value(req, "value", value, sizeof(value))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing value");
    if (!strcmp(value, "me")) g_expected = ExpectedMode::ME;
    else if (!strcmp(value, "other")) g_expected = ExpectedMode::OTHER;
    else if (!strcmp(value, "off")) g_expected = ExpectedMode::OFF;
    else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad expected value");
    reset_test_counters();
    ESP_LOGI(TAG, "test expected=%s", expected_name(g_expected));
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t rec_threshold_handler(httpd_req_t *req) {
    char value[16] = {};
    if (!get_query_value(req, "value", value, sizeof(value))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing value");
    char *end = nullptr;
    const float v = strtof(value, &end);
    if (end == value || *end != '\0' || v < 0.30f || v > 0.90f) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "threshold 0.30..0.90");
    g_recognition_threshold = v;
    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.recognition_threshold = v;
    taskEXIT_CRITICAL(&g_metrics_mux);
    reset_test_counters();
    ESP_LOGI(TAG, "recognition threshold=%.2f", static_cast<double>(v));
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t reset_test_handler(httpd_req_t *req) {
    reset_test_counters();
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t metrics_handler(httpd_req_t *req) {
    Metrics m;
    taskENTER_CRITICAL(&g_metrics_mux);
    memcpy(&m, &g_metrics, sizeof(m));
    taskEXIT_CRITICAL(&g_metrics_mux);
    const uint32_t psram_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    const uint32_t psram_largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    const uint32_t internal_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    char json[4096];
    int n = snprintf(json, sizeof(json),
        "{\"firmware\":\"%s\",\"detector\":\"ESPDet-224\",\"feature_model\":\"MFN_S8_V1\",\"detector_threshold\":%.2f"
        ",\"recognition_threshold\":%.3f,\"expected\":\"%s\",\"identity\":\"%s\""
        ",\"sensor_pid\":%" PRIu32 ",\"jpeg_bytes\":%" PRIu32 ",\"capture_ms\":%" PRIu32
        ",\"camera_frame_ms\":%" PRIu32 ",\"stream_frame_ms\":%" PRIu32
        ",\"pool_drops\":%" PRIu32 ",\"jpeg_rejects\":%" PRIu32 ",\"jpeg_trimmed\":%" PRIu32
        ",\"decode_ms\":%" PRIu32 ",\"detect_ms\":%" PRIu32 ",\"recognition_ms\":%" PRIu32 ",\"ai_total_ms\":%" PRIu32
        ",\"faces\":%d,\"largest_face_w\":%" PRIu32 ",\"largest_face_h\":%" PRIu32
        ",\"enrollment_count\":%d,\"similarity_max\":%.5f,\"similarity_avg\":%.5f"
        ",\"test_frames\":%" PRIu32 ",\"face_frames\":%" PRIu32 ",\"recognition_attempts\":%" PRIu32
        ",\"correct_classifications\":%" PRIu32 ",\"landmark_failures\":%" PRIu32
        ",\"internal_free\":%" PRIu32 ",\"psram_free\":%" PRIu32 ",\"psram_largest\":%" PRIu32 ",\"boxes\":[",
        FW_NAME, static_cast<double>(DETECTOR_THRESHOLD), static_cast<double>(m.recognition_threshold), expected_name(g_expected), m.identity,
        m.sensor_pid, m.source_jpeg_bytes, m.capture_ms, m.camera_frame_ms, m.stream_frame_ms,
        m.pool_drops, m.jpeg_rejects, m.jpeg_trimmed, m.decode_ms, m.detect_ms, m.recognition_ms, m.ai_total_ms,
        m.face_count, m.largest_face_w, m.largest_face_h, m.enrollment_count,
        static_cast<double>(m.similarity_max), static_cast<double>(m.similarity_avg),
        m.test_frames, m.face_frames, m.recognition_attempts, m.correct_classifications, m.landmark_failures,
        internal_free, psram_free, psram_largest);
    for (int i = 0; i < m.face_count && n > 0 && n < static_cast<int>(sizeof(json)); ++i) {
        const FaceBox &b = m.boxes[i];
        n += snprintf(json + n, sizeof(json) - n,
                      "%s{\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d,\"score\":%.4f}",
                      i ? "," : "", b.x1, b.y1, b.x2, b.y2, static_cast<double>(b.score));
    }
    if (n <= 0 || n >= static_cast<int>(sizeof(json) - 3)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "metrics overflow");
    json[n++] = ']'; json[n++] = '}'; json[n] = '\0';
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
        if (previous_ms) {
            taskENTER_CRITICAL(&g_metrics_mux);
            g_metrics.stream_frame_ms = now_ms - previous_ms;
            taskEXIT_CRITICAL(&g_metrics_mux);
        }
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
    const httpd_uri_t metrics = {.uri = "/metrics", .method = HTTP_GET, .handler = metrics_handler, .user_ctx = nullptr};
    const httpd_uri_t enroll = {.uri = "/enroll", .method = HTTP_GET, .handler = enroll_handler, .user_ctx = nullptr};
    const httpd_uri_t clear = {.uri = "/clear", .method = HTTP_GET, .handler = clear_handler, .user_ctx = nullptr};
    const httpd_uri_t expected = {.uri = "/expected", .method = HTTP_GET, .handler = expected_handler, .user_ctx = nullptr};
    const httpd_uri_t threshold = {.uri = "/rec_threshold", .method = HTTP_GET, .handler = rec_threshold_handler, .user_ctx = nullptr};
    const httpd_uri_t reset = {.uri = "/reset_test", .method = HTTP_GET, .handler = reset_test_handler, .user_ctx = nullptr};
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &metrics));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &enroll));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &clear));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &expected));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &threshold));
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

} // namespace

extern "C" void app_main(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    ESP_LOGI(TAG, "NEWO2 face recognition product gate v1");
    ESP_LOGI(TAG, "pipeline: OV3660 VGA JPEG -> RGB565BE -> ESPDet-224@0.30 -> MFN_S8_V1");
    ESP_LOGI(TAG, "enrollment is RAM-only, max_samples=%d recognition_threshold=%.2f", MAX_ENROLL_SAMPLES,
             static_cast<double>(DEFAULT_RECOGNITION_THRESHOLD));
    ESP_LOGI(TAG, "PSRAM total=%u free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));

    g_frame_mutex = xSemaphoreCreateMutex();
    g_camera_mutex = xSemaphoreCreateMutex();
    g_rec_mutex = xSemaphoreCreateMutex();
    if (!g_frame_mutex || !g_camera_mutex || !g_rec_mutex) abort();

    g_detector = new HumanFaceDetect(HumanFaceDetect::ESPDET_PICO_224_224_FACE, true);
    g_feat = new HumanFaceFeat(HumanFaceFeat::MFN_S8_V1, true);
    if (!g_detector || !g_feat) abort();
    g_detector->set_score_thr(DETECTOR_THRESHOLD, 0);

    taskENTER_CRITICAL(&g_metrics_mux);
    g_metrics.recognition_threshold = g_recognition_threshold;
    taskEXIT_CRITICAL(&g_metrics_mux);

    ESP_ERROR_CHECK(init_camera());
    ESP_ERROR_CHECK(init_wifi_ap());
    if (xTaskCreatePinnedToCore(capture_task, "recognition_capture", 6144, nullptr, 4, nullptr, 0) != pdPASS) abort();
    if (xTaskCreatePinnedToCore(ai_task, "recognition_ai", 16384, nullptr, 3, nullptr, 1) != pdPASS) abort();
    ESP_ERROR_CHECK(start_web_servers());
    ESP_LOGI(TAG, "READY: face recognition test at http://192.168.4.1/ ; stream :81/stream");
}
