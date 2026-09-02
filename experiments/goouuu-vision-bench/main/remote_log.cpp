#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "goouuu_remote_log";
constexpr size_t LOG_CAPACITY = 64 * 1024;
constexpr uint16_t LOG_HTTP_PORT = 82;
constexpr uint16_t LOG_CTRL_PORT = 32770;

char *g_log_ring = nullptr;
size_t g_log_head = 0;
size_t g_log_len = 0;
portMUX_TYPE g_log_mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t g_previous_vprintf = nullptr;
httpd_handle_t g_log_httpd = nullptr;

const char kLogUi[] = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover" />
<title>Newo GOOUUU Logs</title>
<style>
:root{color-scheme:dark;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
body{margin:0;background:#090909;color:#eee}main{max-width:1100px;margin:auto;padding:14px}
h1{font-size:17px;margin:0 0 6px}.sub{font-size:11px;opacity:.65;margin-bottom:12px;line-height:1.4}
.toolbar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px}button,a{background:#151515;color:#eee;border:1px solid #333;padding:8px 11px;border-radius:8px;font:inherit;text-decoration:none;cursor:pointer}
button.active{border-color:#eee;background:#242424}#status{font-size:11px;opacity:.65;margin:8px 0}
pre{margin:0;border:1px solid #2a2a2a;border-radius:10px;background:#050505;padding:10px;min-height:60vh;max-height:78vh;overflow:auto;white-space:pre-wrap;word-break:break-word;font-size:11px;line-height:1.35}
</style>
</head>
<body><main>
<h1>Newo / GOOUUU remote logs</h1>
<div class="sub">64 KB in-memory log ring. It captures ESP-IDF logs even when TTL serial is not connected. The buffer lasts until reboot/power loss.</div>
<div class="toolbar">
<button onclick="refreshLogs()">Refresh</button>
<button id="liveBtn" onclick="toggleLive()">Live: OFF</button>
<a href="/download">Download .txt</a>
<button onclick="clearLogs()">Clear</button>
</div>
<div id="status">waiting</div>
<pre id="log">Press Refresh, or enable Live.</pre>
<script>
const out=document.getElementById('log');const status=document.getElementById('status');const liveBtn=document.getElementById('liveBtn');let live=false;let timer=null;
async function refreshLogs(){try{const r=await fetch('/logs?t='+Date.now(),{cache:'no-store'});const t=await r.text();out.textContent=t||'(log buffer empty)';status.textContent=`${t.length} bytes · ${new Date().toLocaleTimeString()}`;out.scrollTop=out.scrollHeight;}catch(e){status.textContent='log fetch failed';}}
function toggleLive(){live=!live;liveBtn.textContent='Live: '+(live?'ON':'OFF');liveBtn.classList.toggle('active',live);if(timer){clearInterval(timer);timer=null;}if(live){refreshLogs();timer=setInterval(refreshLogs,2000);}}
async function clearLogs(){await fetch('/clear',{cache:'no-store'});await refreshLogs();}
</script>
</main></body></html>
)HTML";

void append_log_bytes(const char *data, size_t len) {
    if (!g_log_ring || !data || len == 0) return;

    if (len > LOG_CAPACITY) {
        data += len - LOG_CAPACITY;
        len = LOG_CAPACITY;
    }

    portENTER_CRITICAL(&g_log_mux);
    for (size_t i = 0; i < len; ++i) {
        g_log_ring[g_log_head] = data[i];
        g_log_head = (g_log_head + 1) % LOG_CAPACITY;
        if (g_log_len < LOG_CAPACITY) ++g_log_len;
    }
    portEXIT_CRITICAL(&g_log_mux);
}

int capture_vprintf(const char *format, va_list args) {
    int console_result = 0;
    if (g_previous_vprintf) {
        va_list console_args;
        va_copy(console_args, args);
        console_result = g_previous_vprintf(format, console_args);
        va_end(console_args);
    }

    char line[768];
    va_list capture_args;
    va_copy(capture_args, args);
    const int wanted = vsnprintf(line, sizeof(line), format, capture_args);
    va_end(capture_args);

    if (wanted > 0) {
        const size_t actual = static_cast<size_t>(wanted) < sizeof(line)
                                  ? static_cast<size_t>(wanted)
                                  : sizeof(line) - 1;
        append_log_bytes(line, actual);
        if (static_cast<size_t>(wanted) >= sizeof(line)) {
            static constexpr char truncated[] = " [truncated]\n";
            append_log_bytes(truncated, sizeof(truncated) - 1);
        }
    }
    return console_result;
}

size_t copy_log_snapshot(char *out, size_t capacity) {
    if (!out || capacity == 0 || !g_log_ring) return 0;

    portENTER_CRITICAL(&g_log_mux);
    const size_t count = (g_log_len < capacity) ? g_log_len : capacity;
    const size_t start = (g_log_head + LOG_CAPACITY - g_log_len) % LOG_CAPACITY;
    for (size_t i = 0; i < count; ++i) {
        out[i] = g_log_ring[(start + i) % LOG_CAPACITY];
    }
    portEXIT_CRITICAL(&g_log_mux);
    return count;
}

void clear_log_ring() {
    portENTER_CRITICAL(&g_log_mux);
    g_log_head = 0;
    g_log_len = 0;
    portEXIT_CRITICAL(&g_log_mux);
}

esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kLogUi, HTTPD_RESP_USE_STRLEN);
}

esp_err_t send_log_snapshot(httpd_req_t *req, bool attachment) {
    char *snapshot = static_cast<char *>(heap_caps_malloc(LOG_CAPACITY + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!snapshot) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "unable to allocate log snapshot");
    }

    const size_t count = copy_log_snapshot(snapshot, LOG_CAPACITY);
    snapshot[count] = '\0';
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (attachment) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=goouuu-logs.txt");
    }
    const esp_err_t result = httpd_resp_send(req, snapshot, count);
    heap_caps_free(snapshot);
    return result;
}

esp_err_t logs_handler(httpd_req_t *req) { return send_log_snapshot(req, false); }
esp_err_t download_handler(httpd_req_t *req) { return send_log_snapshot(req, true); }

esp_err_t clear_handler(httpd_req_t *req) {
    clear_log_ring();
    return httpd_resp_sendstr(req, "ok");
}

void log_server_task(void *) {
    // Main firmware initializes lwIP/Wi-Fi in app_main(). Retry until that is ready.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (g_log_httpd) break;

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = LOG_HTTP_PORT;
        config.ctrl_port = LOG_CTRL_PORT;
        config.max_uri_handlers = 4;
        config.lru_purge_enable = true;
        config.stack_size = 6144;

        const esp_err_t err = httpd_start(&g_log_httpd, &config);
        if (err != ESP_OK) {
            g_log_httpd = nullptr;
            continue;
        }

        const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = nullptr};
        const httpd_uri_t logs = {.uri = "/logs", .method = HTTP_GET, .handler = logs_handler, .user_ctx = nullptr};
        const httpd_uri_t download = {.uri = "/download", .method = HTTP_GET, .handler = download_handler, .user_ctx = nullptr};
        const httpd_uri_t clear = {.uri = "/clear", .method = HTTP_GET, .handler = clear_handler, .user_ctx = nullptr};
        httpd_register_uri_handler(g_log_httpd, &root);
        httpd_register_uri_handler(g_log_httpd, &logs);
        httpd_register_uri_handler(g_log_httpd, &download);
        httpd_register_uri_handler(g_log_httpd, &clear);

        ESP_LOGI(TAG, "Remote log page ready: http://192.168.4.1:%u/", static_cast<unsigned>(LOG_HTTP_PORT));
        break;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "uptime=%llds psram_free=%u internal_free=%u log=%u/%u",
                 static_cast<long long>(esp_timer_get_time() / 1000000LL),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(g_log_len),
                 static_cast<unsigned>(LOG_CAPACITY));
    }
}

struct RemoteLogBootstrap {
    RemoteLogBootstrap() {
        g_log_ring = static_cast<char *>(heap_caps_malloc(LOG_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!g_log_ring) return;
        memset(g_log_ring, 0, LOG_CAPACITY);
        g_previous_vprintf = esp_log_set_vprintf(capture_vprintf);
        xTaskCreate(log_server_task, "remote_log", 6144, nullptr, 2, nullptr);
    }
};

RemoteLogBootstrap g_remote_log_bootstrap;

}  // namespace
