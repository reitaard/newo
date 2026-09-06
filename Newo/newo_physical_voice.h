#pragma once

#include <stdint.h>

namespace NewoPhysicalVoice {

enum class TriggerDecision : uint8_t { ACCEPT, DUPLICATE, VOICE_ACTIVE, SPEAKER_BUSY };

class TriggerGate {
 public:
  TriggerDecision decide(uint32_t handshake, bool voiceActive, bool speakerBusy) {
    if (handshake == 0 || handshake == handledHandshake_) return TriggerDecision::DUPLICATE;
    handledHandshake_ = handshake;
    if (voiceActive) return TriggerDecision::VOICE_ACTIVE;
    if (speakerBusy) return TriggerDecision::SPEAKER_BUSY;
    return TriggerDecision::ACCEPT;
  }

 private:
  uint32_t handledHandshake_ = 0;
};

enum class LedState : uint8_t { IDLE, LISTENING, THINKING, SPEAKING, ERROR };

inline const char* ledPayload(LedState state) {
  switch (state) {
    case LedState::LISTENING: return "on";
    case LedState::THINKING: return "blink_fast";
    case LedState::SPEAKING: return "blink_slow";
    case LedState::ERROR: return "error";
    case LedState::IDLE: return "off";
  }
  return "off";
}

inline const char* ledName(LedState state) {
  switch (state) {
    case LedState::LISTENING: return "listening";
    case LedState::THINKING: return "thinking";
    case LedState::SPEAKING: return "speaking";
    case LedState::ERROR: return "error";
    case LedState::IDLE: return "idle";
  }
  return "idle";
}

}  // namespace NewoPhysicalVoice
