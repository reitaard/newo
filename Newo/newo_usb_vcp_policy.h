#pragma once

#include <stdint.h>

namespace NewoUsbVcpPolicy {
constexpr uint16_t kWchVid = 0x1a86;
constexpr uint16_t kCh340Pid = 0x7523;

inline bool guardInitialAutoReset(uint16_t vid, uint16_t pid) {
  return vid == kWchVid && pid == kCh340Pid;
}
}  // namespace NewoUsbVcpPolicy
