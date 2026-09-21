#pragma once

#include <Arduino.h>

// Local wake acknowledgement. This is deliberately independent from the
// network /speaker path so hearing the cue is a trustworthy "start speaking"
// boundary even while /voice TLS is still connecting.
const char* newoWakeEarconModeName();
bool newoSetWakeEarconMode(const char* mode);
bool newoPlayWakeEarcon();
uint32_t newoWakeChimeFrames();
int16_t newoWakeChimeSample(uint32_t frame, int32_t amplitude);
