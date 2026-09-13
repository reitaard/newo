#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>

namespace NewoMemoryDiagnostics {

inline void log(const char* checkpoint) {
  Serial.printf(
      "[memory] %s free_heap=%lu min_free_heap=%lu internal_free=%lu "
      "internal_largest=%lu free_psram=%lu\n",
      checkpoint ? checkpoint : "unknown",
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getMinFreeHeap()),
      static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned long>(ESP.getFreePsram()));
}

}  // namespace NewoMemoryDiagnostics
