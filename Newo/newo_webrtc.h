#pragma once
#include <cstdint>
#include "esp_agc.h"
// Arduino-ESP32 3.3.10 omits the private sr_ringbuf.h included by its public
// WebRTC header. Keep ESP-SR opaque and declare only the linked C ABI we use.
struct NewoWebRtcHandle;
extern "C" {
NewoWebRtcHandle* webrtc_create(int, int, agc_mode_t, int, int, int);
int16_t* webrtc_process(NewoWebRtcHandle*, int16_t*, int*, bool, bool);
void webrtc_destroy(NewoWebRtcHandle*);
}
