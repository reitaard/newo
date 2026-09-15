#include "newo2_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace Newo2Events {
namespace {
QueueHandle_t g_queue = nullptr;
portMUX_TYPE g_seq_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t g_sequence = 0;
}

bool begin() {
    if (g_queue) return true;
    g_queue = xQueueCreate(16, sizeof(Event));
    return g_queue != nullptr;
}

bool publish(const Event &event, uint32_t timeout_ms) {
    if (!g_queue) return false;
    return xQueueSend(g_queue, &event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool receive(Event &event, uint32_t timeout_ms) {
    if (!g_queue) return false;
    return xQueueReceive(g_queue, &event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

uint32_t next_sequence() {
    taskENTER_CRITICAL(&g_seq_mux);
    const uint32_t value = ++g_sequence;
    taskEXIT_CRITICAL(&g_seq_mux);
    return value;
}
}  // namespace Newo2Events
