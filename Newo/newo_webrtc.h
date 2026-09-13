#pragma once

#include <cstdint>

#include "esp_agc.h"

// Arduino-ESP32 3.3.10 ships esp_sr_webrtc.h with a dependency on the private
// sr_ringbuf.h header, but that header is absent from the packaged S3 SDK.
// Newo treats the implementation as opaque and declares only the three C ABI
// functions it uses; their implementations remain provided by ESP-SR.
struct NewoWebRtcHandle;

extern "C" {
NewoWebRtcHandle* webrtc_create(
    int frameLengthMs, int nsMode, agc_mode_t agcMode, int agcGain,
    int agcTargetLevel, int sampleRate);
int16_t* webrtc_process(
    NewoWebRtcHandle* handle, int16_t* input, int* outputSamples,
    bool enableNs, bool enableAgc);
void webrtc_destroy(NewoWebRtcHandle* handle);
}
