#include "newo2_network.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "newo2_event.h"

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
EventGroupHandle_t g_wifi_events = nullptr;
esp_websocket_client_handle_t g_ws = nullptr;
volatile bool g_cloud_connected = false;
char g_ws_uri[192] = {};
char g_ws_headers[256] = {};

void send_json(cJSON *root) {
    if (!root || !g_ws || !g_cloud_connected) return;
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return;
    esp_websocket_client_send_text(g_ws, text, strlen(text), pdMS_TO_TICKS(1000));
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
        ESP_LOGW(TAG, "cloud disconnected");
    } else if (event_id == WEBSOCKET_EVENT_DATA && data && data->op_code == 0x1 && data->data_len > 0) {
        handle_command(data->data_ptr, data->data_len);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        g_cloud_connected = false;
        ESP_LOGW(TAG, "cloud websocket error");
    }
}

void wifi_event(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED);
        g_cloud_connected = false;
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
    g_ws = esp_websocket_client_init(&cfg);
    if (!g_ws) {
        ESP_LOGE(TAG, "websocket init failed");
        vTaskDelete(nullptr);
        return;
    }
    esp_websocket_register_events(g_ws, WEBSOCKET_EVENT_ANY, ws_event, nullptr);
    esp_websocket_client_start(g_ws);
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
}  // namespace

bool begin() {
#if !NEWO2_HAS_SECRETS
    ESP_LOGW(TAG, "newo2_secrets.h missing; cloud/Wi-Fi disabled");
    return false;
#else
    if (!Newo2Secrets::WIFI_SSID[0] || strlen(Newo2Secrets::DEVICE_SECRET) < 24) {
        ESP_LOGW(TAG, "Wi-Fi/device credentials incomplete; cloud disabled");
        return false;
    }
    g_wifi_events = xEventGroupCreate();
    if (!g_wifi_events) return false;
    ESP_ERROR_CHECK(esp_netif_init());
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(loop_err);
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    wifi_config_t wifi = {};
    strlcpy(reinterpret_cast<char *>(wifi.sta.ssid), Newo2Secrets::WIFI_SSID, sizeof(wifi.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi.sta.password), Newo2Secrets::WIFI_PASSWORD, sizeof(wifi.sta.password));
    wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi.sta.pmf_cfg.capable = true;
    wifi.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
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

void send_status(bool camera_enabled, bool motion_enabled) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");
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
