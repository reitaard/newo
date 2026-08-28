#pragma once

#include <stdint.h>

enum class VoiceHealthState : uint8_t { HEALTHY, DEGRADED, RESETTING, COOLDOWN };
enum class VoiceHealthReason : uint8_t { NONE, QUEUE, SEND_LATENCY, DROPS };

struct VoiceHealthDecision {
  bool degraded = false;
  bool recovered = false;
  bool reset = false;
  VoiceHealthReason reason = VoiceHealthReason::NONE;
};

// Deliberately dependency-free so the transition rules can be host-tested.
class NewoVoiceHealth {
 public:
  NewoVoiceHealth(uint8_t queueThreshold, uint32_t sendWarnUs, uint32_t sendFatalUs,
                  uint32_t sustainMs, uint32_t cooldownMs)
      : queueThreshold_(queueThreshold), sendWarnUs_(sendWarnUs), sendFatalUs_(sendFatalUs),
        sustainMs_(sustainMs), cooldownMs_(cooldownMs) {}

  VoiceHealthDecision observe(uint32_t nowMs, uint8_t queueDepth, uint32_t dropped,
                              uint32_t overruns, uint32_t sendDurationUs) {
    const uint32_t dropDelta = dropped - lastDropped_;
    const uint32_t overrunDelta = overruns - lastOverruns_;
    lastDropped_ = dropped;
    lastOverruns_ = overruns;
    if (state_ == VoiceHealthState::COOLDOWN) {
      if (static_cast<int32_t>(nowMs - cooldownUntilMs_) < 0) return {};
      state_ = VoiceHealthState::HEALTHY;
    }

    VoiceHealthReason reason = VoiceHealthReason::NONE;
    if (queueDepth >= queueThreshold_) reason = VoiceHealthReason::QUEUE;
    if (sendDurationUs != 0) {
      if (sendDurationUs >= sendWarnUs_) {
        ++warnStreak_;
        if (sendDurationUs >= sendFatalUs_ || warnStreak_ >= 2) reason = VoiceHealthReason::SEND_LATENCY;
      } else {
        warnStreak_ = 0;
      }
    }
    if (queueDepth >= queueThreshold_ && (dropDelta > 0 || overrunDelta > 0)) {
      reason = VoiceHealthReason::DROPS;
    }

    if (state_ == VoiceHealthState::HEALTHY && reason != VoiceHealthReason::NONE) {
      state_ = VoiceHealthState::DEGRADED;
      degradedSinceMs_ = nowMs;
      return {true, false, false, reason};
    }
    if (state_ == VoiceHealthState::DEGRADED) {
      if (reason == VoiceHealthReason::NONE) {
        state_ = VoiceHealthState::HEALTHY;
        warnStreak_ = 0;
        return {false, true, false, VoiceHealthReason::NONE};
      }
      if (static_cast<uint32_t>(nowMs - degradedSinceMs_) >= sustainMs_) {
        state_ = VoiceHealthState::RESETTING;
        return {false, false, true, reason};
      }
    }
    return {};
  }

  void resetComplete(uint32_t nowMs) {
    state_ = VoiceHealthState::COOLDOWN;
    cooldownUntilMs_ = nowMs + cooldownMs_;
    warnStreak_ = 0;
  }
  VoiceHealthState state() const { return state_; }

 private:
  uint8_t queueThreshold_;
  uint32_t sendWarnUs_, sendFatalUs_, sustainMs_, cooldownMs_;
  VoiceHealthState state_ = VoiceHealthState::HEALTHY;
  uint32_t degradedSinceMs_ = 0, cooldownUntilMs_ = 0;
  uint32_t lastDropped_ = 0, lastOverruns_ = 0;
  uint8_t warnStreak_ = 0;
};
