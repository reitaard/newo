#pragma once

#include <stdint.h>

namespace NanoResetPolicy {
constexpr uint8_t kPowerOn = 1u << 0;
constexpr uint8_t kExternal = 1u << 1;
constexpr uint8_t kBrownout = 1u << 2;
constexpr uint8_t kWatchdog = 1u << 3;

inline const char* causeName(uint8_t flags) {
  if (flags & kExternal) return "external";
  if (flags & kWatchdog) return "watchdog";
  if (flags & kBrownout) return "brownout";
  if (flags & kPowerOn) return "power_on";
  return "unknown";
}

class TriggerOnce {
 public:
  void arm(uint8_t flags) { pending_ = (flags & kExternal) != 0; }
  bool take() {
    if (!pending_) return false;
    pending_ = false;
    return true;
  }

 private:
  bool pending_ = false;
};
}  // namespace NanoResetPolicy
