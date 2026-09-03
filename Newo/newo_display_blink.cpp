#include "newo_display.h"

namespace {
constexpr uint32_t kWinkBurstMs = 240;
constexpr uint32_t kWinkCalmMinMs = 2'000;
constexpr uint32_t kWinkCalmMaxMs = 4'001;
constexpr uint8_t kForceLongBlink = 2;
}  // namespace

void NewoDisplay::updateBlinkBeforeFrame(uint32_t now, NewoDisplayMode activeMode) {
  const bool winkStyle = activeMode == NewoDisplayMode::IDLE &&
                         (faceStyle_ == NewoFaceStyle::WINK_LEFT || faceStyle_ == NewoFaceStyle::WINK_RIGHT);

  if (winkActive_ && now - winkStartedMs_ >= kWinkBurstMs) {
    winkActive_ = false;
    nextWinkMs_ = now + static_cast<uint32_t>(random(kWinkCalmMinMs, kWinkCalmMaxMs));
    nextBlinkMs_ = now + static_cast<uint32_t>(random(2'500, 5'501));
  }

  const bool deliberatelyClosed = activeMode == NewoDisplayMode::IDLE &&
      (faceStyle_ == NewoFaceStyle::CLOSED || faceStyle_ == NewoFaceStyle::DETACHED ||
       faceStyle_ == NewoFaceStyle::SLEEPING);
  const bool autonomousEpisodeActive = autonomousEpisode_ != AutonomousEpisode::WAITING;

  if (blinkPhase_ != BlinkPhase::OPEN || winkActive_) return;

  if ((autonomousEpisode_ == AutonomousEpisode::LOW_ENERGY ||
       autonomousEpisode_ == AutonomousEpisode::DROWSY_REST) &&
      autonomousEpisodeBlinkRequested_ && !autonomousEpisodeBlinkStarted_) {
    startBilateralBlink(false, kForceLongBlink);
    autonomousEpisodeBlinkStarted_ = true;
    return;
  }

  if (!autonomousEpisodeActive && winkStyle && static_cast<int32_t>(now - nextWinkMs_) >= 0) {
    blinkSchedulerState_ = BlinkSchedulerState::WAITING;
    longBlink_ = false;
    postSaccadeBlinkPending_ = false;
    winkActive_ = true;
    winkLeft_ = faceStyle_ == NewoFaceStyle::WINK_LEFT;
    winkStartedMs_ = now;
    return;
  }

  if (!autonomousEpisodeActive && !deliberatelyClosed && static_cast<int32_t>(now - nextBlinkMs_) >= 0) {
    const bool doubleSecond = blinkSchedulerState_ == BlinkSchedulerState::DOUBLE_SECOND;
    startBilateralBlink(activeMode == NewoDisplayMode::IDLE && autoFaceEnabled_ &&
                        !postSaccadeBlinkPending_ && !doubleSecond);
  }
}

void NewoDisplay::advanceBlinkAfterFrame(uint32_t now) {
  if (blinkPhase_ == BlinkPhase::OPEN) return;
  if (--blinkFramesRemaining_ != 0) return;

  if (blinkPhase_ == BlinkPhase::HALF_CLOSED) {
    blinkPhase_ = BlinkPhase::CLOSED;
    blinkFramesRemaining_ = longBlink_ ? 3 : 1;
    return;
  }

  if (blinkPhase_ == BlinkPhase::CLOSED) {
    blinkPhase_ = BlinkPhase::HALF_OPEN;
    blinkFramesRemaining_ = 1;
    return;
  }

  blinkPhase_ = BlinkPhase::OPEN;
  longBlink_ = false;
  if (blinkSchedulerState_ == BlinkSchedulerState::DOUBLE_PAUSE) {
    blinkSchedulerState_ = BlinkSchedulerState::DOUBLE_SECOND;
    nextBlinkMs_ = now + static_cast<uint32_t>(random(120, 251));
  } else {
    blinkSchedulerState_ = BlinkSchedulerState::WAITING;
    scheduleNextBilateralBlink(now);
  }
}
