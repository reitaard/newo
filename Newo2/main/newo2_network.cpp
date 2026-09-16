#include "newo2_network.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "newo2_event.h"
#include "newo2_console.h"
#include "newo2_camera.h"

#ifndef CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK
#error "Newo2 requires CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK for sustained video TX"
#endif
#if CONFIG_LWIP_TCP_SND_BUF_DEFAULT < 65535
#error "Newo2 requires CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535 for 20 FPS uplink"
#endif
#if CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM < 64
#error "Newo2 requires at least 64 Wi-Fi dynamic TX buffers for 20 FPS uplink"
#endif
#if CONFIG_ESP_WIFI_TX_BA_WIN < 32
#error "Newo2 requires Wi-Fi TX BA window >= 32 for 20 FPS uplink"
#endif

#if __has_include("newo2_secrets.h")
#include "newo2_secrets.h"
#define NEWO2_HAS_SECRETS 1
#else
namespace Newo2Secrets {
inline constexpr char WIFI_SSID[] = "";
inline constexpr char WIFI_PASSWORD[] = "";
inline constexpr char DEVICE_ID[] = "newo2-01";
inline constexpr char DEVICE_SECRET[] = "";
inline constexpr char CLOUD_HOST[] = "newo.reitaard.de";
}
#define NEWO2_HAS_SECRETS 0
#endif

namespace Newo2Network {
namespace {
constexpr const char *TAG = "newo2_network";
constexpr EventBits_t WIFI_CONNECTED = BIT0;
// 64 VGA JPEG packets absorb roughly three seconds of normal 20 FPS Wi-Fi jitter
// without blocking the fixed-rate camera/SD recorder.
constexpr UBaseType_t kVideoQueueDepth = 64;
constexpr uint32_t kRecordingDrainTimeoutMs = 7000;
// esp_websocket_client defaults to a 1 KiB buffer, which fragments every camera
// JPEG into many TLS/WebSocket writes. A 64 KiB transport buffer covers normal
// VGA frames in one write and keeps the sender ahead of the 20 FPS producer.
constexpr size_t kWebSocketBufferSize = 64 * 1024;
constexpr uint32_t kVideoSendTimeoutMs = 2000;

struct VideoQueueItem {
    uint8_t *packet;
    size_t length;
    bool recording;
};

EventGroupHandle_t g_wifi_events = nullptr;
QueueHandle_t g_video_queue = nullptr;
SemaphoreHandle_t g_ws_send_mutex = nullptr;
esp_websocket_client_handle_t g_ws = nullptr;
volatile bool g_cloud_connected = false;
char g_ws_uri[192] = {};
char g_ws_headers[256] = {};
uint32_t g_serial_sequence = 0;
std::atomic<bool> g_recording_transport_active{false};
std::atomic<uint32_t> g_recording_frames_pending{0};
std::atomic<uint32_t> g_video_queue_drops{0};
std::atomic<uint32_t> g_video_send_failures{0};

int send_binary_serialized(const uint8_t *data, size_t len, TickType_t lock_wait, TickType_t send_timeout) {
    if (!data || !len || !g_ws_send_mutex) return -1;
    if (xSemaphoreTake(g_ws_send_mutex, lock_wait) != pdTRUE) return -1;
    int sent = -1;
    if (g_ws && g_cloud_connected) {
        sent = esp_websocket_client_send_bin(
            g_ws, reinterpret_cast<const char *>(data), len, send_timeout);
    }
    xSemaphoreGive(g_ws_send_mutex);
    return sent;
}

int send_text_serialized(const char *text, size_t len, TickType_t lock_wait, TickType_t send_timeout) {
    if (!text || !len || !g_ws_send_mutex) return -1;
    if (xSemaphoreTake(g_ws_send_mutex, lock_wait) != pdTRUE) return -1;
    int sent = -1;
    if (g_ws && g_cloud_connected) sent = esp_websocket_client_send_text(g_ws, text, len, send_timeout);
    xSemaphoreGive(g_ws_send_mutex);
    return sent;
}

void release_video_item(VideoQueueItem &item) {
    if (item.packet) heap_caps_free(item.packet);
    if (item.recording) g_recording_frames_pending.fetch_sub(1, std::memory_order_relaxed);
    item.packet = nullptr;
    item.length = 0;
    item.recording = false;
}

void clear_video_queue() {
    if (!g_video_queue) return;
    VideoQueueItem item = {};
    while (xQueueReceive(g_video_queue, &item, 0) == pdTRUE) release_video_item(item);
}

bool wait_for_recording_frames(uint32_t timeout_ms) {
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000LL;
    while (g_recording_frames_pending.load(std::memory_order_relaxed) != 0 && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(5));
    return g_recording_frames_pending.load(std::memory_order_relaxed) == 0;
}

void video_sender_task(void *) {
    for (;;) {
        VideoQueueItem item = {};
        if (!g_video_queue || xQueueReceive(g_video_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        bool ok = false;
        if (item.packet && item.length && g_ws && g_cloud_connected) {
            const int sent = send_binary_serialized(
                item.packet, item.length, portMAX_DELAY, pdMS_TO_TICKS(kVideoSendTimeoutMs));
            ok = sent == static_cast<int>(item.length);
        }
        if (!ok && item.recording) {
            const uint32_t failures = g_video_send_failures.fetch_add(1, std::memory_order_relaxed) + 1;
            if (failures == 1 || failures % 20 == 0)
                ESP_LOGW(TAG, "recording frame network send failed count=%lu",
                         static_cast<unsigned long>(failures));
        }
        release_video_item(item);
    }
}

void drain_serial_monitor() {
    if (!g_ws || !g_cloud_connected || !Newo2Console::remote_enabled()) return;

    // /2 is diagnostic traffic and must never compete with recording video.
    // Keep capturing into its PSRAM ring, but drain it only after video TX catches up.
    if (g_recording_transport_active.load(std::memory_order_relaxed) ||
        (g_video_queue && uxQueueMessagesWaiting(g_video_queue) > 0)) return;

    static uint8_t frame[12 + 1024];
    uint32_t dropped = 0;
    const size_t bytes = Newo2Console::read_remote(frame + 12, sizeof(frame) - 12, &dropped);
    if (!bytes && !dropped) return;
    frame[0] = 'N'; frame[1] = 'S'; frame[2] = 'M'; frame[3] = '2';
    const uint32_t sequence = ++g_serial_sequence;
    for (uint8_t i = 0; i < 4; ++i) {
        frame[4 + i] = static_cast<uint8_t>(sequence >> (i * 8));
        frame[8 + i] = static_cast<uint8_t>(dropped >> (i * 8));
    }
    const int sent = send_binary_serialized(frame, bytes + 12, 0, pdMS_TO_TICKS(100));
    if (sent < 0) Newo2Console::note_remote_drop(bytes);
}

void send_json(cJSON *root) {
    if (!root || !g_ws || !g_cloud_connected) return;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return;
    const int sent = send_text_serialized(text, strlen(text), pdMS_TO_TICKS(1500), pdMS_TO_TICKS(1500));
    if (sent < 0) ESP_LOGW(TAG, "control websocket send failed");
    cJSON_free(text);
}

void copy_string(char *dst, size_t size, const cJSON *item) {
    if (!dst || !size) return;
    dst[0] = '\0';
    if (cJSON_IsString(item) && item->valuestring) strlcpy(dst, item->valuestring, size);
}

void handle_command(const char *data, int len) {
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const char *kind = cJSON_IsString(type) ? type->valuestring : "";
    Newo2Events::Event event = {};
    copy_string(event.request_id, sizeof(event.request_id), request_id);

    if (strcmp(kind, "camera_control") == 0 || strcmp(kind, "motion_control") == 0) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (cJSON_IsBool(enabled) && event.request_id[0]) {
            event.type = strcmp(kind, "camera_control") == 0 ? Newo2Events::Type::CAMERA_SET : Newo2Events::Type::MOTION_SET;
            event.enabled = cJSON_IsTrue(enabled);
            strlcpy(event.source, "vps", sizeof(event.source));
            Newo2Events::publish(event, 20);
        }
    } else if (strcmp(kind, "snapshot_capture") == 0 && event.request_id[0]) {
        event.type = Newo2Events::Type::SNAPSHOT_REQUEST;
        strlcpy(event.source, "vps", sizeof(event.source));
        Newo2Events::publish(event, 20);
    } else if (strcmp(kind, "status_request") == 0) {
        event.type = Newo2Events::Type::STATUS_REQUEST;
        strlcpy(event.source, "vps", sizeof(event.source));
        Newo2Events::publish(event, 20);
    } else if (strcmp(kind, "stream_control") == 0 && event.request_id[0]) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (cJSON_IsBool(enabled)) {
            event.type = Newo2Events::Type::STREAM_SET;
            event.enabled = cJSON_IsTrue(enabled);
            strlcpy(event.source, "vps", sizeof(event.source));
            Newo2Events::publish(event, 20);
        }
    } else if (strcmp(kind, "record_start") == 0 && event.request_id[0]) {
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration_seconds");
        event.type = Newo2Events::Type::RECORD_START;
        event.duration_seconds = cJSON_IsNumber(duration) && duration->valuedouble >= 0
            ? static_cast<uint32_t>(duration->valuedouble) : 30;
        strlcpy(event.source, "vps", sizeof(event.source));
        Newo2Events::publish(event, 20);
    } else if (strcmp(kind, "record_stop") == 0 && event.request_id[0]) {
        event.type = Newo2Events::Type::RECORD_STOP;
        strlcpy(event.source, "vps", sizeof(event.source));
        Newo2Events::publish(event, 20);
    } else if (strcmp(kind, "settings_control") == 0 && event.request_id[0]) {
        const cJSON *setting = cJSON_GetObjectItemCaseSensitive(root, "setting");
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (cJSON_IsString(setting) && cJSON_IsString(target) && (cJSON_IsString(value) || cJSON_IsNumber(value))) {
            event.type = Newo2Events::Type::SETTINGS_SET;
            copy_string(event.setting, sizeof(event.setting), setting);
            copy_string(event.source, sizeof(event.source), target);
            if (cJSON_IsString(value)) copy_string(event.value, sizeof(event.value), value);
            else snprintf(event.value, sizeof(event.value), "%d", value->valueint);
            Newo2Events::publish(event, 20);
        }
    } else if (strcmp(kind, "settings_request") == 0 && event.request_id[0]) {
        send_camera_settings(event.request_id, true);
    } else if (strcmp(kind, "serial_monitor_control") == 0 && event.request_id[0]) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if (cJSON_IsBool(enabled)) {
            const bool applied = Newo2Console::set_remote_enabled(cJSON_IsTrue(enabled));
            send_serial_monitor_ack(event.request_id, Newo2Console::remote_enabled(), applied);
        }
    }
    cJSON_Delete(root);
}

void ws_event(void *, esp_event_base_t, int32_t event_id, void *event_data) {
    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        g_cloud_connected = true;
        cJSON *hello = cJSON_CreateObject();
        cJSON_AddStringToObject(hello, "type", "hello");
        cJSON_AddStringToObject(hello, "device", Newo2Secrets::DEVICE_ID);
        cJSON_AddStringToObject(hello, "role", "camera");
        cJSON_AddStringToObject(hello, "firmware", "newo2-1.0.0");
        cJSON_AddNumberToObject(hello, "protocol", 1);
        send_json(hello);
        cJSON_Delete(hello);
        ESP_LOGI(TAG, "cloud connected");
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        g_cloud_connected = false;
        clear_video_queue();
        ESP_LOGW(TAG, "cloud disconnected");
    } else if (event_id == WEBSOCKET_EVENT_DATA && data && data->op_code == 0x1 && data->data_len > 0) {
        handle_command(data->data_ptr, data->data_len);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        g_cloud_connected = false;
        clear_video_queue();
        ESP_LOGW(TAG, "cloud websocket error");
    }
}

void wifi_event(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED);
        g_cloud_connected = false;
        clear_video_queue();
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED);
    }
}

void cloud_task(void *) {
    xEventGroupWaitBits(g_wifi_events, WIFI_CONNECTED, pdFALSE, pdFALSE, portMAX_DELAY);
    snprintf(g_ws_uri, sizeof(g_ws_uri), "wss://%s/newo2/device", Newo2Secrets::CLOUD_HOST);
    snprintf(g_ws_headers, sizeof(g_ws_headers), "X-Newo-Device-Id: %s\r\nAuthorization: Bearer %s\r\n",
             Newo2Secrets::DEVICE_ID, Newo2Secrets::DEVICE_SECRET);
    esp_websocket_client_config_t cfg = {};
    cfg.uri = g_ws_uri;
    cfg.headers = g_ws_headers;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.reconnect_timeout_ms = 3000;
    cfg.network_timeout_ms = 10000;
    cfg.buffer_size = kWebSocketBufferSize;
    ESP_LOGI(TAG, "video transport ws_buf=%u tcp_snd=%u wifi_tx_buf=%u tx_ba=%u",
             static_cast<unsigned>(kWebSocketBufferSize),
             static_cast<unsigned>(CONFIG_LWIP_TCP_SND_BUF_DEFAULT),
             static_cast<unsigned>(CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM),
             static_cast<unsigned>(CONFIG_ESP_WIFI_TX_BA_WIN));
    g_ws = esp_websocket_client_init(&cfg);
    if (!g_ws) {
        ESP_LOGE(TAG, "websocket init failed");
        vTaskDelete(nullptr);
        return;
    }
    esp_websocket_register_events(g_ws, WEBSOCKET_EVENT_ANY, ws_event, nullptr);
    esp_websocket_client_start(g_ws);
    for (;;) {
        drain_serial_monitor();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}  // namespace

bool begin() {
#if !NEWO2_HAS_SECRETS
    ESP_LOGW(TAG, "newo2_secrets.h missing; cloud/Wi-Fi disabled");
    return false;
#else
    if (strlen(Newo2Secrets::DEVICE_SECRET) < 24) {
        ESP_LOGW(TAG, "device credential incomplete; cloud disabled");
        return false;
    }
    g_wifi_events = xEventGroupCreate();
    g_video_queue = xQueueCreate(kVideoQueueDepth, sizeof(VideoQueueItem));
    g_ws_send_mutex = xSemaphoreCreateMutex();
    if (!g_wifi_events || !g_video_queue || !g_ws_send_mutex) return false;
    if (xTaskCreate(video_sender_task, "newo2_video_tx", 4096, nullptr, 5, nullptr) != pdPASS) {
        vQueueDelete(g_video_queue);
        g_video_queue = nullptr;
        return false;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(loop_err);
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (Newo2Secrets::WIFI_SSID[0]) {
        wifi_config_t wifi = {};
        strlcpy(reinterpret_cast<char *>(wifi.sta.ssid), Newo2Secrets::WIFI_SSID, sizeof(wifi.sta.ssid));
        strlcpy(reinterpret_cast<char *>(wifi.sta.password), Newo2Secrets::WIFI_PASSWORD, sizeof(wifi.sta.password));
        wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi.sta.pmf_cfg.capable = true;
        wifi.sta.pmf_cfg.required = false;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    } else {
        wifi_config_t saved = {};
        ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &saved));
        if (!saved.sta.ssid[0]) {
            ESP_LOGW(TAG, "no compiled or saved Wi-Fi credentials; cloud disabled");
            return false;
        }
        ESP_LOGI(TAG, "using Wi-Fi credentials already saved in NVS");
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    xTaskCreate(cloud_task, "newo2_cloud", 6144, nullptr, 4, nullptr);
    return true;
#endif
}

bool cloud_connected() { return g_cloud_connected; }

void send_control_ack(const char *request_id, const char *target, bool enabled, bool applied) {
    if (!request_id || !request_id[0]) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "control_ack");
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "target", target);
    cJSON_AddBoolToObject(root, "enabled", enabled);
    cJSON_AddBoolToObject(root, "applied", applied);
    send_json(root);
    cJSON_Delete(root);
}

void send_status(bool camera_enabled, bool motion_enabled, const char *request_id) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");
    if (request_id && request_id[0]) cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddBoolToObject(root, "camera_enabled", camera_enabled);
    cJSON_AddBoolToObject(root, "motion_enabled", motion_enabled);
    cJSON_AddBoolToObject(root, "cloud_connected", g_cloud_connected);
    send_json(root);
    cJSON_Delete(root);
}

void send_motion_detected(uint32_t sequence, float confidence) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "motion_detected");
    cJSON_AddNumberToObject(root, "sequence", sequence);
    cJSON_AddNumberToObject(root, "confidence", confidence);
    send_json(root);
    cJSON_Delete(root);
}

void send_serial_monitor_ack(const char *request_id, bool enabled, bool applied) {
    if (!request_id || !request_id[0]) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "serial_monitor_ack");
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddBoolToObject(root, "enabled", enabled);
    cJSON_AddBoolToObject(root, "applied", applied);
    cJSON_AddNumberToObject(root, "capacity_bytes", Newo2Console::remote_capacity());
    send_json(root);
    cJSON_Delete(root);
}

void send_media_ack(const char *request_id, const char *target, bool enabled, bool applied, uint8_t fps) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "media_ack");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    cJSON_AddStringToObject(root, "target", target ? target : "media");
    cJSON_AddBoolToObject(root, "enabled", enabled);
    cJSON_AddBoolToObject(root, "applied", applied);
    cJSON_AddNumberToObject(root, "fps", fps);
    send_json(root);
    cJSON_Delete(root);
}

void send_camera_settings(const char *request_id, bool applied) {
    if (!request_id || !request_id[0]) return;
    const Newo2Camera::Settings settings = Newo2Camera::settings();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "settings_ack");
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddBoolToObject(root, "applied", applied);
    cJSON_AddStringToObject(root, "photo_resolution", settings.photo_resolution);
    cJSON_AddStringToObject(root, "video_resolution", settings.video_resolution);
    cJSON_AddNumberToObject(root, "photo_quality", settings.photo_quality);
    cJSON_AddNumberToObject(root, "video_quality", settings.video_quality);
    send_json(root);
    cJSON_Delete(root);
}

void send_record_result(const char *request_id, bool success, uint32_t frames, uint32_t dropped,
                        size_t bytes, uint32_t duration_ms, const char *reason) {
    // The recorder is already stopped here, so waiting cannot disturb its 20 FPS
    // capture cadence. Drain queued recording frames before record_complete so the
    // VPS does not finalize the MP4 before the tail of the MJPEG stream arrives.
    const bool drained = wait_for_recording_frames(kRecordingDrainTimeoutMs);
    if (!drained) {
        ESP_LOGW(TAG, "recording network drain timed out pending=%lu",
                 static_cast<unsigned long>(g_recording_frames_pending.load(std::memory_order_relaxed)));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "record_complete");
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "");
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddNumberToObject(root, "frames", frames);
    cJSON_AddNumberToObject(root, "dropped", dropped);
    cJSON_AddNumberToObject(root, "bytes", static_cast<double>(bytes));
    cJSON_AddNumberToObject(root, "duration_ms", duration_ms);
    cJSON_AddStringToObject(root, "reason", reason ? reason : "unknown");
    send_json(root);
    cJSON_Delete(root);
    g_recording_transport_active.store(false, std::memory_order_relaxed);
}

bool send_video_frame(const uint8_t *jpeg, size_t len, uint16_t width, uint16_t height,
                      uint32_t sequence, uint8_t fps, bool streaming, bool recording) {
    if (recording && !g_recording_transport_active.exchange(true, std::memory_order_relaxed)) {
        g_video_queue_drops.store(0, std::memory_order_relaxed);
        g_video_send_failures.store(0, std::memory_order_relaxed);
    }
    if (!jpeg || !len || !g_video_queue || !g_ws || !g_cloud_connected || len > 256 * 1024) return false;
    uint8_t *packet = static_cast<uint8_t *>(heap_caps_malloc(24 + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!packet) return false;
    packet[0] = 'N'; packet[1] = '2'; packet[2] = 'J'; packet[3] = 'F';
    packet[4] = (streaming ? 1 : 0) | (recording ? 2 : 0); packet[5] = 0;
    packet[6] = width & 0xff; packet[7] = width >> 8;
    packet[8] = height & 0xff; packet[9] = height >> 8;
    packet[10] = fps; packet[11] = 0;
    const uint32_t timestamp_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    for (uint8_t i = 0; i < 4; ++i) {
        packet[12 + i] = static_cast<uint8_t>(sequence >> (i * 8));
        packet[16 + i] = static_cast<uint8_t>(timestamp_ms >> (i * 8));
        packet[20 + i] = static_cast<uint8_t>(len >> (i * 8));
    }
    memcpy(packet + 24, jpeg, len);

    VideoQueueItem item = {packet, 24 + len, recording};
    if (recording) g_recording_frames_pending.fetch_add(1, std::memory_order_relaxed);
    if (xQueueSend(g_video_queue, &item, 0) != pdTRUE) {
        if (recording) g_recording_frames_pending.fetch_sub(1, std::memory_order_relaxed);
        heap_caps_free(packet);
        const uint32_t drops = g_video_queue_drops.fetch_add(1, std::memory_order_relaxed) + 1;
        if (drops == 1 || drops % 20 == 0)
            ESP_LOGW(TAG, "video network queue saturated drops=%lu",
                     static_cast<unsigned long>(drops));
        return false;
    }
    return true;
}

void send_snapshot_result(const char *request_id, const char *source, uint32_t sequence,
                          size_t bytes, bool saved, bool uploaded, bool captured) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", captured ? "snapshot_captured" : "snapshot_error");
    if (request_id && request_id[0]) cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "source", source && source[0] ? source : "unknown");
    cJSON_AddNumberToObject(root, "sequence", sequence);
    cJSON_AddNumberToObject(root, "bytes", static_cast<double>(bytes));
    cJSON_AddBoolToObject(root, "saved", saved);
    cJSON_AddBoolToObject(root, "uploaded", uploaded);
    send_json(root);
    cJSON_Delete(root);
}

bool upload_snapshot(const uint8_t *jpeg, size_t len, const char *request_id,
                     const char *source, uint32_t sequence) {
    if (!jpeg || !len || !g_wifi_events || !(xEventGroupGetBits(g_wifi_events) & WIFI_CONNECTED)) return false;
    char url[320] = {};
    snprintf(url, sizeof(url), "https://%s/newo2/snapshot?request_id=%s&source=%s&sequence=%lu",
             Newo2Secrets::CLOUD_HOST, request_id && request_id[0] ? request_id : "",
             source && source[0] ? source : "unknown", static_cast<unsigned long>(sequence));
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    char bearer[192] = {};
    snprintf(bearer, sizeof(bearer), "Bearer %s", Newo2Secrets::DEVICE_SECRET);
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(client, "X-Newo-Device-Id", Newo2Secrets::DEVICE_ID);
    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_post_field(client, reinterpret_cast<const char *>(jpeg), static_cast<int>(len));
    const esp_err_t err = esp_http_client_perform(client);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    const bool ok = err == ESP_OK && status >= 200 && status < 300;
    if (!ok) ESP_LOGW(TAG, "snapshot upload failed err=%s status=%d", esp_err_to_name(err), status);
    return ok;
}
}  // namespace Newo2Network
