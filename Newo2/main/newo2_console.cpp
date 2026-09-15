#include "newo2_console.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace Newo2Console {
namespace {
uint8_t *g_buffer = nullptr;
size_t g_head = 0;
size_t g_count = 0;
uint32_t g_dropped = 0;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t g_previous = nullptr;

void capture(const uint8_t *data, size_t size) {
    if (!data || !size) return;
    portENTER_CRITICAL(&g_mux);
    if (g_buffer) {
        for (size_t i = 0; i < size; ++i) {
            if (g_count == remote_capacity()) {
                g_head = (g_head + 1) % remote_capacity();
                --g_count;
                if (g_dropped != UINT32_MAX) ++g_dropped;
            }
            g_buffer[(g_head + g_count) % remote_capacity()] = data[i];
            ++g_count;
        }
    }
    portEXIT_CRITICAL(&g_mux);
}

int logging_vprintf(const char *format, va_list args) {
    va_list output_args;
    va_copy(output_args, args);
    const int result = g_previous ? g_previous(format, output_args) : vprintf(format, output_args);
    va_end(output_args);

    char line[512];
    va_list capture_args;
    va_copy(capture_args, args);
    const int length = vsnprintf(line, sizeof(line), format, capture_args);
    va_end(capture_args);
    if (length > 0) capture(reinterpret_cast<const uint8_t *>(line), std::min<size_t>(length, sizeof(line) - 1));
    return result;
}
}  // namespace

bool begin() {
    if (!g_previous) g_previous = esp_log_set_vprintf(logging_vprintf);
    return true;
}

bool set_remote_enabled(bool enabled) {
    if (enabled && !g_buffer) {
        uint8_t *allocated = static_cast<uint8_t *>(heap_caps_malloc(remote_capacity(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!allocated) return false;
        portENTER_CRITICAL(&g_mux);
        if (!g_buffer) {
            g_buffer = allocated;
            allocated = nullptr;
            g_head = g_count = 0;
            g_dropped = 0;
        }
        portEXIT_CRITICAL(&g_mux);
        if (allocated) heap_caps_free(allocated);
    } else if (!enabled) {
        uint8_t *released = nullptr;
        portENTER_CRITICAL(&g_mux);
        released = g_buffer;
        g_buffer = nullptr;
        g_head = g_count = 0;
        g_dropped = 0;
        portEXIT_CRITICAL(&g_mux);
        if (released) heap_caps_free(released);
    }
    return true;
}

bool remote_enabled() {
    portENTER_CRITICAL(&g_mux);
    const bool enabled = g_buffer != nullptr;
    portEXIT_CRITICAL(&g_mux);
    return enabled;
}

size_t read_remote(uint8_t *destination, size_t capacity, uint32_t *dropped_bytes) {
    if (!destination || !capacity) return 0;
    portENTER_CRITICAL(&g_mux);
    if (dropped_bytes) { *dropped_bytes = g_dropped; g_dropped = 0; }
    const size_t copied = std::min(capacity, g_count);
    for (size_t i = 0; i < copied; ++i) destination[i] = g_buffer[(g_head + i) % remote_capacity()];
    g_head = (g_head + copied) % remote_capacity();
    g_count -= copied;
    portEXIT_CRITICAL(&g_mux);
    return copied;
}

void note_remote_drop(size_t bytes) {
    portENTER_CRITICAL(&g_mux);
    const uint32_t bounded = bytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(bytes);
    g_dropped = UINT32_MAX - g_dropped < bounded ? UINT32_MAX : g_dropped + bounded;
    portEXIT_CRITICAL(&g_mux);
}
}  // namespace Newo2Console
