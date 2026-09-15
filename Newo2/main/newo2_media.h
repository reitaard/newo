#pragma once

#include <cstdint>

namespace Newo2Media {
bool begin();
bool set_streaming(bool enabled, const char *request_id);
bool start_recording(uint32_t duration_seconds, const char *request_id);
bool stop_recording(const char *request_id);
bool streaming();
bool recording();
uint8_t effective_fps();
}  // namespace Newo2Media
