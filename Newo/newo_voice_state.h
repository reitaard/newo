#pragma once

#include <Arduino.h>

// The wake-only audio lifecycle. ARMED owns I2S through ESP_SR; STREAMING
// pauses ESP_SR before the streaming task reads I2S, so there is never a
// second concurrent I2S consumer.
enum class NewoVoiceState : uint8_t { OFF, ARMED, STREAMING };

inline const char* newoVoiceStateName(NewoVoiceState state) {
  switch (state) {
    case NewoVoiceState::OFF: return "off";
    case NewoVoiceState::ARMED: return "armed";
    case NewoVoiceState::STREAMING: return "streaming";
  }
  return "off";
}
