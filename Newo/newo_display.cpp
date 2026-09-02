#include "newo_display.h"

#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <SPI.h>
#include <cmath>
#include <cstring>

#include "newo_config.h"

namespace {
constexpr uint16_t kWidth = 240;
constexpr uint16_t kHeight = 240;
constexpr uint32_t kTemporaryMs = 7'000;
constexpr uint32_t kEcoPageMs = 5'000;
constexpr uint32_t kNormalFaceFrameMs = 50;    // ~20 FPS.
constexpr uint32_t kSpeakingFaceFrameMs = 120;  // ~8.3 FPS while audio has priority.
constexpr uint32_t kWinkBurstMs = 240;
constexpr uint32_t kWinkCalmMinMs = 2'000;
constexpr uint32_t kWinkCalmMaxMs = 4'001;
constexpr uint16_t kWhite = ST77XX_WHITE;
constexpr int16_t kEyeCanvasWidth = 200;
constexpr int16_t kEyeCanvasHeight = 82;
constexpr int16_t kEyeCanvasX = 20;
constexpr int16_t kEyeCanvasY = 40;
constexpr uint32_t kAutonomousStateUpdateMs = 3'000;
constexpr uint32_t kAutonomousStateLogMs = 60'000;
constexpr uint32_t kInactivityBeforeDriftMs = 30'000;
constexpr uint32_t kRelaxedInactivityMs = 120'000;
constexpr uint32_t kDrowsyInactivityMs = 300'000;
constexpr uint32_t kAutonomousEpisodeMinMs = 8'000;
constexpr uint32_t kAutonomousEpisodeMaxMs = 18'000;
constexpr int16_t kAutonomousGazeHardX = 20;
constexpr int16_t kAutonomousGazeHardY = 10;
constexpr uint8_t kForceDoubleBlink = 1;
constexpr uint8_t kForceLongBlink = 2;
constexpr uint8_t kCuriosityBaseline = 42;

bool isOngoingEngagement(NewoDisplayMode mode) {
  return mode == NewoDisplayMode::LISTENING || mode == NewoDisplayMode::THINKING ||
         mode == NewoDisplayMode::SPEAKING;
}

uint8_t saturatingAdd(uint8_t value, uint8_t amount) {
  const uint16_t result = static_cast<uint16_t>(value) + amount;
  return result > 100 ? 100 : static_cast<uint8_t>(result);
}

void formatUptime(char* out, size_t size, uint32_t ms) {
  const uint32_t seconds = ms / 1000;
  snprintf(out, size, "%luh %02lum", static_cast<unsigned long>(seconds / 3600),
           static_cast<unsigned long>((seconds / 60) % 60));
}

int16_t easeAutonomousGaze(int16_t current, int16_t target) {
  const int16_t delta = target - current;
  const int16_t magnitude = delta < 0 ? -delta : delta;
  if (magnitude < 2) return target;
  int16_t step = magnitude / 2;
  if (step > 8) step = 8;
  if (step < 1) step = 1;
  return current + (delta > 0 ? step : -step);
}
}  // namespace

NewoDisplay::NewoDisplay()
    : display_(NewoConfig::DISPLAY_CS_PIN, NewoConfig::DISPLAY_DC_PIN, NewoConfig::DISPLAY_RST_PIN) {}

void NewoDisplay::begin() {
  SPI.begin(NewoConfig::DISPLAY_SCK_PIN, -1, NewoConfig::DISPLAY_MOSI_PIN, NewoConfig::DISPLAY_CS_PIN);
  display_.init(kWidth, kHeight);
  display_.setRotation(3);
  display_.setTextColor(kWhite);
  display_.setTextWrap(false);
  const uint32_t now = millis();
  initializeAutonomousState(now);
  syncEffectiveMode(now);
  render();
}

bool NewoDisplay::setMode(NewoDisplayMode mode, const char* text, bool temporary) {
  if (mode > NewoDisplayMode::ECO) return false;
  const uint32_t now = millis();
  if (mode_ != mode) {
    mode_ = mode;
    modeStartedMs_ = now;
    if (mode_ == NewoDisplayMode::ERROR) noteError();
  }
  mode_ = mode;
  strncpy(text_, text ? text : "", sizeof(text_) - 1);
  text_[sizeof(text_) - 1] = '\0';
  if (!temporary && mode != NewoDisplayMode::ECO) {
    ecoEnabled_ = false;
    ecoPage_ = 0;
    nextEcoPageMs_ = 0;
    persistentMode_ = mode;
    strncpy(persistentText_, text_, sizeof(persistentText_) - 1);
    persistentText_[sizeof(persistentText_) - 1] = '\0';
  }
  temporary_ = temporary;
  restoreAtMs_ = temporary ? now + kTemporaryMs : 0;
  syncEffectiveMode(now);
  dirty_ = true;
  return true;
}

void NewoDisplay::setListeningActive(bool active) {
  if (active == listeningActive_) return;
  listeningActive_ = active;
  syncEffectiveMode(millis());
}

void NewoDisplay::setAssistantThinking(bool active) {
  if (active == assistantThinking_) return;
  assistantThinking_ = active;
  syncEffectiveMode(millis());
}

void NewoDisplay::setSpeakerActive(bool active) {
  if (active == speakerActive_) return;
  speakerActive_ = active;
  syncEffectiveMode(millis());
}

void NewoDisplay::noteSystemError() {
  const uint32_t now = millis();
  errorActive_ = true;
  errorUntilMs_ = now + 1'500;
  noteError();
  syncEffectiveMode(now);
}

void NewoDisplay::setClockEnabled(bool enabled) {
  if (clockEnabled_ == enabled) return;
  clockEnabled_ = enabled;
  // updateClock() clears or redraws only its own lower face-view region.
}

bool NewoDisplay::setFaceStyle(NewoFaceStyle style) {
  if (style > NewoFaceStyle::SLEEPY) return false;
  const bool styleChanged = style != faceStyle_;
  faceStyle_ = style;
  autoFaceEnabled_ = style == NewoFaceStyle::NEUTRAL;
  mode_ = NewoDisplayMode::IDLE;
  persistentMode_ = NewoDisplayMode::IDLE;
  text_[0] = '\0';
  persistentText_[0] = '\0';
  temporary_ = false;
  restoreAtMs_ = 0;
  ecoEnabled_ = false;
  ecoPage_ = 0;
  nextEcoPageMs_ = 0;
  const uint32_t now = millis();
  modeStartedMs_ = now;
  if (styleChanged) noteInteraction(now, 1, 2, 2);
  syncEffectiveMode(now);
  resetFaceMotion(now);
  dirty_ = true;
  return true;
}

void NewoDisplay::toggleEco() {
  const uint32_t now = millis();
  temporary_ = false;
  if (!ecoEnabled_) {
    ecoEnabled_ = true;
    mode_ = NewoDisplayMode::ECO;
    resetAutonomousEpisode(now);
    text_[0] = '\0';
    ecoPage_ = 0;
    nextEcoPageMs_ = now + kEcoPageMs;
  } else {
    ecoEnabled_ = false;
    mode_ = persistentMode_;
    resetFaceMotion(now);
    strncpy(text_, persistentText_, sizeof(text_) - 1);
      text_[sizeof(text_) - 1] = '\0';
  }
  syncEffectiveMode(now);
  dirty_ = true;
}

void NewoDisplay::updateTelemetry(bool wifiConnected, int32_t rssi, bool cloudConnected, uint32_t uptimeMs,
                                  uint32_t freeHeap, uint32_t freePsram, const NewoLog::Stats& logs) {
  telemetry_ = {wifiConnected, rssi, cloudConnected, uptimeMs, freeHeap, freePsram, logs};
}

void NewoDisplay::loop() {
  const uint32_t now = millis();
  if (temporary_ && static_cast<int32_t>(now - restoreAtMs_) >= 0) {
    temporary_ = false;
    mode_ = ecoEnabled_ ? NewoDisplayMode::ECO : persistentMode_;
    if (!ecoEnabled_) {
      resetFaceMotion(now);
      strncpy(text_, persistentText_, sizeof(text_) - 1);
      text_[sizeof(text_) - 1] = '\0';
    }
    dirty_ = true;
  }
  if (errorActive_ && static_cast<int32_t>(now - errorUntilMs_) >= 0) {
    errorActive_ = false;
    errorUntilMs_ = 0;
  }
  syncEffectiveMode(now);
  if (ecoEnabled_ && !temporary_ && static_cast<int32_t>(now - nextEcoPageMs_) >= 0) {
    nextEcoPageMs_ = now + kEcoPageMs;
    ecoPage_ = (ecoPage_ + 1) % 3;
    dirty_ = true;
  }
  updateAutonomousState(now);
  if (dirty_) render();
  if (!temporary_ && mode_ != NewoDisplayMode::ECO && mode_ != NewoDisplayMode::MESSAGE &&
      static_cast<int32_t>(now - nextFaceFrameMs_) >= 0) {
    // Speaker playback owns the SPEAKING context and uses a slower bounded
    // face-frame cadence while audio is active.
    nextFaceFrameMs_ = now + (speakerActive_ ? kSpeakingFaceFrameMs : kNormalFaceFrameMs);
    drawFaceFrame(now);
  }
}

const char* NewoDisplay::statusFor(NewoDisplayMode mode) {
  switch (mode) {
    case NewoDisplayMode::LISTENING: return "LISTENING";
    case NewoDisplayMode::THINKING: return "THINKING";
    case NewoDisplayMode::SPEAKING: return "SPEAKING";
    case NewoDisplayMode::ERROR: return "ERROR";
    default: return "";
  }
}

const char* NewoDisplay::contextName(NewoDisplayMode mode) const {
  if (mode == NewoDisplayMode::IDLE) return autoFaceEnabled_ ? "IDLE_AUTO" : "IDLE_MANUAL";
  if (mode == NewoDisplayMode::MESSAGE) return "MESSAGE";
  if (mode == NewoDisplayMode::ECO) return "ECO";
  return statusFor(mode);
}

const char* NewoDisplay::episodeName(AutonomousEpisode episode) {
  switch (episode) {
    case AutonomousEpisode::CURIOUS_SCAN: return "CURIOUS_SCAN";
    case AutonomousEpisode::LOW_ENERGY: return "LOW_ENERGY";
    case AutonomousEpisode::SOCIAL_ATTENTION: return "SOCIAL_ATTENTION";
    case AutonomousEpisode::ALERT_CHECK: return "ALERT_CHECK";
    case AutonomousEpisode::WAITING: return "WAITING";
  }
  return "UNKNOWN";
}

void NewoDisplay::recordGazeTarget(uint16_t holdMs) {
  ++eyeGazeEvents_;
  const int16_t absoluteX = gazeTargetX_ < 0 ? -gazeTargetX_ : gazeTargetX_;
  const int16_t absoluteY = gazeTargetY_ < 0 ? -gazeTargetY_ : gazeTargetY_;
  if (absoluteX >= 8 || absoluteY >= 3) ++eyeMeaningfulGazeEvents_;
  const char* direction = absoluteX >= 8 ? (gazeTargetX_ < 0 ? "LEFT" : "RIGHT")
      : absoluteY >= 3 ? (gazeTargetY_ < 0 ? "UP" : "DOWN") : "CENTER";
  Serial.printf("[EYES] gaze=%s x=%d y=%d hold_ms=%u\n", direction,
                static_cast<int>(gazeTargetX_), static_cast<int>(gazeTargetY_),
                static_cast<unsigned>(holdMs));
}

void NewoDisplay::maybeLogEyeStats(uint32_t now) {
  if (static_cast<int32_t>(now - nextAutonomousStateLogMs_) < 0) return;
  Serial.printf("[EYES_STATS] context=%s context_changes=%lu energy=%u curiosity=%u social=%u stress=%u "
                "gaze=%lu meaningful_gaze=%lu blinks=%lu double_blinks=%lu long_blinks=%lu "
                "episodes=%lu completed=%lu errors=%lu\n",
                contextName(effectiveMode(now)), static_cast<unsigned long>(eyeContextChanges_),
                static_cast<unsigned>(energy_),
                static_cast<unsigned>(curiosity_), static_cast<unsigned>(social_), static_cast<unsigned>(stress_),
                static_cast<unsigned long>(eyeGazeEvents_), static_cast<unsigned long>(eyeMeaningfulGazeEvents_),
                static_cast<unsigned long>(eyeBlinkEvents_), static_cast<unsigned long>(eyeDoubleBlinkEvents_),
                static_cast<unsigned long>(eyeLongBlinkEvents_), static_cast<unsigned long>(eyeEpisodeStarts_),
                static_cast<unsigned long>(eyeEpisodeCompletions_), static_cast<unsigned long>(eyeErrorEvents_));
  nextAutonomousStateLogMs_ = now + kAutonomousStateLogMs;
}

void NewoDisplay::render() {
  dirty_ = false;
  display_.fillScreen(ST77XX_BLACK);
  if (mode_ == NewoDisplayMode::ECO) return drawEco();
  if (temporary_) return drawTextPage("", text_, true);
  if (mode_ == NewoDisplayMode::MESSAGE) return drawMessage();
  drawFace();
  drawFaceResponse();
}

void NewoDisplay::drawFace() {
  // Face states are visual-only: RoboEyes plus the compact activity animation below them.
  if (nextBlinkMs_ == 0) resetFaceMotion(millis());
  nextFaceFrameMs_ = 0;
  drawFaceFrame(millis());
}

void NewoDisplay::resetFaceMotion(uint32_t now) {
  blinkPhase_ = BlinkPhase::OPEN;
  blinkSchedulerState_ = BlinkSchedulerState::WAITING;
  blinkFramesRemaining_ = 0;
  longBlink_ = false;
  postSaccadeBlinkPending_ = false;
  if (autonomousIdle()) {
    scheduleNextBilateralBlink(now);
  } else {
    // Keep non-autonomous mode/style entry timing unchanged.
    nextBlinkMs_ = now + 1'000;
  }
  nextWinkMs_ = now + static_cast<uint32_t>(random(kWinkCalmMinMs, kWinkCalmMaxMs));
  winkStartedMs_ = 0;
  winkActive_ = false;
  winkLeft_ = false;
  autonomousGazePhase_ = AutonomousGazePhase::CHOOSE_TARGET;
  fixationUntilMs_ = 0;
  microCorrectionAtMs_ = 0;
  microCorrectionPending_ = false;
  autonomousGazeLargeShift_ = false;
  gazeX_ = 0;
  gazeY_ = 0;
  gazeTargetX_ = 0;
  gazeTargetY_ = 0;
  nextGazeMs_ = now;
  nextFaceFrameMs_ = 0;
  resetAutonomousEpisode(now);
}

NewoDisplayMode NewoDisplay::effectiveMode(uint32_t now) const {
  if (ecoEnabled_ || mode_ == NewoDisplayMode::ECO) return NewoDisplayMode::ECO;
  if (temporary_ || mode_ == NewoDisplayMode::MESSAGE) return NewoDisplayMode::MESSAGE;
  if (mode_ == NewoDisplayMode::ERROR ||
      (errorActive_ && static_cast<int32_t>(now - errorUntilMs_) < 0)) {
    return NewoDisplayMode::ERROR;
  }
  if (listeningActive_ || mode_ == NewoDisplayMode::LISTENING) return NewoDisplayMode::LISTENING;
  if (speakerActive_ || mode_ == NewoDisplayMode::SPEAKING) return NewoDisplayMode::SPEAKING;
  if (assistantThinking_ || mode_ == NewoDisplayMode::THINKING) return NewoDisplayMode::THINKING;
  return NewoDisplayMode::IDLE;
}

void NewoDisplay::syncEffectiveMode(uint32_t now) {
  const NewoDisplayMode next = effectiveMode(now);
  const bool nextAutoFace = next == NewoDisplayMode::IDLE && autoFaceEnabled_;
  if (next == lastEffectiveMode_ && nextAutoFace == lastEffectiveAutoFace_) return;

  lastEffectiveMode_ = next;
  lastEffectiveAutoFace_ = nextAutoFace;
  ++eyeContextChanges_;
  Serial.printf("[EYES] context=%s\n", contextName(next));
  modeStartedMs_ = now;
  switch (next) {
    case NewoDisplayMode::LISTENING:
      noteInteraction(now, 5, 10, 12);
      break;
    case NewoDisplayMode::THINKING:
      noteInteraction(now, 2, 4, 5);
      break;
    case NewoDisplayMode::SPEAKING:
      noteInteraction(now, 4, 5, 10);
      break;
    default:
      break;
  }
  resetFaceMotion(now);
  dirty_ = true;
}

void NewoDisplay::initializeAutonomousState(uint32_t now) {
  energy_ = 70;
  curiosity_ = kCuriosityBaseline;
  social_ = 38;
  stress_ = 5;
  idleDriftTicks_ = 0;
  lastInteractionMs_ = now;
  nextAutonomousStateMs_ = now + kAutonomousStateUpdateMs;
  nextAutonomousStateLogMs_ = now + kAutonomousStateLogMs;
}

void NewoDisplay::noteInteraction(uint32_t now, uint8_t energyGain, uint8_t curiosityGain, uint8_t socialGain) {
  lastInteractionMs_ = now;
  idleDriftTicks_ = 0;
  energy_ = saturatingAdd(energy_, energyGain);
  curiosity_ = saturatingAdd(curiosity_, curiosityGain);
  social_ = saturatingAdd(social_, socialGain);
  if (stress_ > 0) --stress_;
}

void NewoDisplay::noteError() {
  // A single completed ERROR remains just above the subtle neutral-IDLE threshold briefly.
  ++eyeErrorEvents_;
  stress_ = saturatingAdd(stress_, 12);
}

void NewoDisplay::updateAutonomousState(uint32_t now) {
  if (static_cast<int32_t>(now - nextAutonomousStateMs_) >= 0) {
    nextAutonomousStateMs_ = now + kAutonomousStateUpdateMs;
    if (curiosity_ > kCuriosityBaseline) --curiosity_;
    if (curiosity_ < kCuriosityBaseline) ++curiosity_;
    if (stress_ > 0) --stress_;

    const NewoDisplayMode activeMode = effectiveMode(now);
    if (isOngoingEngagement(activeMode)) {
      // Engagement is current rather than an accumulating score. Entry events add
      // a small spark; this keeps the live social signal high only while active.
      lastInteractionMs_ = now;
      idleDriftTicks_ = 0;
      const uint8_t socialTarget = activeMode == NewoDisplayMode::SPEAKING ? 82
          : activeMode == NewoDisplayMode::LISTENING ? 78 : 68;
      if (social_ < socialTarget) social_ = static_cast<uint8_t>(social_ + (socialTarget - social_ > 1 ? 2 : 1));
    } else {
      if (social_ > 30) --social_;
      if (now - lastInteractionMs_ >= kInactivityBeforeDriftMs && ++idleDriftTicks_ >= 10) {
        idleDriftTicks_ = 0;
        if (energy_ > 35) --energy_;
      } else if (now - lastInteractionMs_ < kInactivityBeforeDriftMs) {
        idleDriftTicks_ = 0;
      }
    }
  }

  maybeLogEyeStats(now);
}

void NewoDisplay::resetAutonomousEpisode(uint32_t now) {
  autonomousEpisode_ = AutonomousEpisode::WAITING;
  autonomousEpisodeHoldMs_ = 0;
  autonomousEpisodeStep_ = 0;
  autonomousEpisodeDirection_ = 1;
  autonomousEpisodeBlinkRequested_ = false;
  autonomousEpisodeBlinkStarted_ = false;
  if (autonomousIdle()) scheduleNextAutonomousEpisode(now);
  else nextAutonomousEpisodeMs_ = 0;
}

void NewoDisplay::scheduleNextAutonomousEpisode(uint32_t now) {
  if (!autonomousIdle()) {
    nextAutonomousEpisodeMs_ = 0;
    return;
  }

  int32_t minimumMs = kAutonomousEpisodeMinMs;
  int32_t maximumMs = kAutonomousEpisodeMaxMs;
  const uint32_t inactiveMs = now - lastInteractionMs_;
  if (inactiveMs >= kDrowsyInactivityMs) {
    minimumMs += 10'000;
    maximumMs += 14'000;
  } else if (inactiveMs >= kRelaxedInactivityMs) {
    minimumMs += 4'000;
    maximumMs += 6'000;
  }
  nextAutonomousEpisodeMs_ = now + static_cast<uint32_t>(random(minimumMs, maximumMs + 1));
}

void NewoDisplay::finishAutonomousEpisode(uint32_t now) {
  const AutonomousEpisode completedEpisode = autonomousEpisode_;
  if (completedEpisode != AutonomousEpisode::WAITING) {
    ++eyeEpisodeCompletions_;
    Serial.printf("[EYES] episode=%s done\n", episodeName(completedEpisode));
  }
  autonomousEpisode_ = AutonomousEpisode::WAITING;
  autonomousEpisodeHoldMs_ = 0;
  autonomousEpisodeStep_ = 0;
  autonomousEpisodeBlinkRequested_ = false;
  autonomousEpisodeBlinkStarted_ = false;
  if (blinkPhase_ == BlinkPhase::OPEN && blinkSchedulerState_ == BlinkSchedulerState::WAITING &&
      static_cast<int32_t>(now - nextBlinkMs_) >= 0) {
    scheduleNextBilateralBlink(now);
  }
  scheduleNextAutonomousEpisode(now);
}

void NewoDisplay::beginAutonomousEpisodeGaze(uint32_t now, int16_t targetX, int16_t targetY,
                                              uint16_t holdMs) {
  gazeTargetX_ = targetX;
  gazeTargetY_ = targetY;
  autonomousGazeLargeShift_ = false;
  microCorrectionPending_ = false;
  microCorrectionAtMs_ = 0;
  autonomousEpisodeHoldMs_ = holdMs;
  recordGazeTarget(holdMs);
  autonomousGazePhase_ = AutonomousGazePhase::MOVING;
  nextGazeMs_ = now;
}

void NewoDisplay::chooseAutonomousEpisode(uint32_t now) {
  if (!autonomousIdle() || blinkPhase_ != BlinkPhase::OPEN || winkActive_ ||
      blinkSchedulerState_ != BlinkSchedulerState::WAITING || postSaccadeBlinkPending_ ||
      static_cast<int32_t>(now - nextBlinkMs_) >= 0) {
    scheduleNextAutonomousEpisode(now);
    return;
  }

  const uint32_t inactiveMs = now - lastInteractionMs_;
  const InactivityStage stage = inactiveMs >= kDrowsyInactivityMs ? InactivityStage::DROWSY
                              : inactiveMs >= kRelaxedInactivityMs ? InactivityStage::RELAXED
                                                                    : InactivityStage::ACTIVE;

  int curiousWeight = 30 + (curiosity_ > kCuriosityBaseline
      ? static_cast<int>(curiosity_ - kCuriosityBaseline) / 2 : 0);
  int socialWeight = 24 + static_cast<int>(social_) / 10;
  int alertWeight = 20 + static_cast<int>(stress_);
  int lowEnergyWeight = 8 + (energy_ < 70 ? static_cast<int>(70 - energy_) : 0);
  if (curiosity_ > 55) curiousWeight += 10;
  if (social_ > 60) socialWeight += 10;
  if (stress_ > 8) alertWeight += 20;
  if (energy_ < 55) lowEnergyWeight += 15;
  if (stage == InactivityStage::RELAXED) {
    if (curiousWeight > 5) curiousWeight -= 5;
    socialWeight += 3;
    lowEnergyWeight += 15;
  } else if (stage == InactivityStage::DROWSY) {
    if (curiousWeight > 10) curiousWeight -= 10;
    if (socialWeight > 5) socialWeight -= 5;
    if (alertWeight > 5) alertWeight -= 5;
    lowEnergyWeight += 35;
  }

  const int totalWeight = curiousWeight + socialWeight + alertWeight + lowEnergyWeight;
  const long choice = random(totalWeight);
  int threshold = curiousWeight;
  if (choice < threshold) autonomousEpisode_ = AutonomousEpisode::CURIOUS_SCAN;
  else if (choice < (threshold += socialWeight)) autonomousEpisode_ = AutonomousEpisode::SOCIAL_ATTENTION;
  else if (choice < (threshold += alertWeight)) autonomousEpisode_ = AutonomousEpisode::ALERT_CHECK;
  else autonomousEpisode_ = AutonomousEpisode::LOW_ENERGY;

  autonomousEpisodeStep_ = 0;
  autonomousEpisodeDirection_ = random(0, 2) == 0 ? -1 : 1;
  autonomousEpisodeBlinkRequested_ = false;
  autonomousEpisodeBlinkStarted_ = false;
  ++eyeEpisodeStarts_;
  Serial.printf("[EYES] episode=%s start\n", episodeName(autonomousEpisode_));

  switch (autonomousEpisode_) {
    case AutonomousEpisode::CURIOUS_SCAN:
      beginAutonomousEpisodeGaze(now, static_cast<int16_t>(autonomousEpisodeDirection_ * random(16, 21)),
                                 static_cast<int16_t>(random(-3, 4)), static_cast<uint16_t>(random(800, 1'301)));
      break;
    case AutonomousEpisode::LOW_ENERGY:
      beginAutonomousEpisodeGaze(now, static_cast<int16_t>(random(-4, 5)),
                                 static_cast<int16_t>(random(4, 10)),
                                 static_cast<uint16_t>(stage == InactivityStage::DROWSY
                                     ? random(2'200, 3'801) : random(1'600, 2'801)));
      break;
    case AutonomousEpisode::SOCIAL_ATTENTION:
      beginAutonomousEpisodeGaze(now, static_cast<int16_t>(random(-3, 4)),
                                 static_cast<int16_t>(random(-2, 3)),
                                 static_cast<uint16_t>(random(1'600, 2'801)));
      break;
    case AutonomousEpisode::ALERT_CHECK:
      beginAutonomousEpisodeGaze(now, static_cast<int16_t>(autonomousEpisodeDirection_ * random(16, 21)),
                                 static_cast<int16_t>(random(-4, 3)), static_cast<uint16_t>(random(500, 901)));
      break;
    case AutonomousEpisode::WAITING:
      break;
  }
}

void NewoDisplay::advanceAutonomousEpisode(uint32_t now) {
  switch (autonomousEpisode_) {
    case AutonomousEpisode::CURIOUS_SCAN:
      if (autonomousEpisodeStep_ == 0) {
        autonomousEpisodeStep_ = 1;
        beginAutonomousEpisodeGaze(now, static_cast<int16_t>(autonomousEpisodeDirection_ * kAutonomousGazeHardX),
                                   static_cast<int16_t>(random(-4, 3)), 550);
      } else if (autonomousEpisodeStep_ == 1) {
        autonomousEpisodeStep_ = 2;
        beginAutonomousEpisodeGaze(now, 0, 0, 850);
      } else {
        finishAutonomousEpisode(now);
      }
      return;
    case AutonomousEpisode::LOW_ENERGY:
      if (autonomousEpisodeStep_ == 0) {
        autonomousEpisodeStep_ = 1;
        autonomousEpisodeBlinkRequested_ = true;
      } else if (autonomousEpisodeStep_ == 2) {
        finishAutonomousEpisode(now);
      }
      return;
    case AutonomousEpisode::SOCIAL_ATTENTION:
      finishAutonomousEpisode(now);
      return;
    case AutonomousEpisode::ALERT_CHECK:
      if (autonomousEpisodeStep_ == 0) {
        autonomousEpisodeStep_ = 1;
        beginAutonomousEpisodeGaze(now, static_cast<int16_t>(-autonomousEpisodeDirection_ * 12),
                                   static_cast<int16_t>(random(-2, 3)), 420);
      } else if (autonomousEpisodeStep_ == 1) {
        autonomousEpisodeStep_ = 2;
        beginAutonomousEpisodeGaze(now, 0, 0, 650);
      } else {
        finishAutonomousEpisode(now);
      }
      return;
    case AutonomousEpisode::WAITING:
      return;
  }
}

void NewoDisplay::updateAutonomousEpisode(uint32_t now) {
  if (!autonomousIdle()) {
    if (autonomousEpisode_ != AutonomousEpisode::WAITING) resetAutonomousEpisode(now);
    return;
  }
  if (autonomousEpisode_ == AutonomousEpisode::WAITING) {
    if (static_cast<int32_t>(now - nextAutonomousEpisodeMs_) >= 0) chooseAutonomousEpisode(now);
    return;
  }
  if (autonomousEpisodeBlinkRequested_) {
    if (autonomousEpisodeBlinkStarted_ && blinkPhase_ == BlinkPhase::OPEN &&
        blinkSchedulerState_ == BlinkSchedulerState::WAITING) {
      autonomousEpisodeBlinkRequested_ = false;
      autonomousEpisodeBlinkStarted_ = false;
      autonomousEpisodeStep_ = 2;
      beginAutonomousEpisodeGaze(now, 0, 0, 1'100);
    }
    return;
  }
  if (autonomousGazePhase_ == AutonomousGazePhase::FIXATING &&
      static_cast<int32_t>(now - fixationUntilMs_) >= 0) {
    advanceAutonomousEpisode(now);
  }
}

uint32_t NewoDisplay::adjustAutonomousFixation(uint32_t fixationMs) const {
  int32_t adjusted = static_cast<int32_t>(fixationMs);
  if (energy_ < 70) adjusted += static_cast<int32_t>(70 - energy_) * 10;
  if (energy_ > 75) adjusted -= static_cast<int32_t>(energy_ - 75) * 7;
  if (curiosity_ > kCuriosityBaseline) adjusted -= static_cast<int32_t>(curiosity_ - kCuriosityBaseline) * 2;
  if (social_ > 60) adjusted += static_cast<int32_t>(social_ - 60) * 3;
  if (stress_ > 0) adjusted -= static_cast<int32_t>(stress_) * 3;
  if (adjusted < 600) return 600;
  if (adjusted > 5'000) return 5'000;
  return static_cast<uint32_t>(adjusted);
}

bool NewoDisplay::autonomousIdle() const {
  return effectiveMode(millis()) == NewoDisplayMode::IDLE && autoFaceEnabled_;
}

void NewoDisplay::scheduleNextBilateralBlink(uint32_t now) {
  if (!autonomousIdle()) {
    nextBlinkMs_ = now + static_cast<uint32_t>(random(2'500, 5'501));
    return;
  }
  int32_t intervalMs = random(2'500, 6'001);
  if (energy_ < 55) intervalMs += static_cast<int32_t>(55 - energy_) * 12;
  if (energy_ > 80) intervalMs -= static_cast<int32_t>(energy_ - 80) * 8;
  if (stress_ > 8) intervalMs -= static_cast<int32_t>(stress_ - 8) * 10;
  if (intervalMs < 2'200) intervalMs = 2'200;
  if (intervalMs > 6'200) intervalMs = 6'200;
  nextBlinkMs_ = now + static_cast<uint32_t>(intervalMs);
}

void NewoDisplay::startBilateralBlink(bool allowAutonomousVariation, uint8_t forcedBlink) {
  uint8_t longBlinkChance = 2;
  if (energy_ < 55) ++longBlinkChance;
  longBlink_ = forcedBlink == kForceLongBlink ||
               (forcedBlink == 0 && allowAutonomousVariation && random(100) < longBlinkChance);
  const bool doubleBlink = forcedBlink == kForceDoubleBlink ||
      (allowAutonomousVariation && !longBlink_ && random(100) < 7);
  blinkSchedulerState_ = doubleBlink ? BlinkSchedulerState::DOUBLE_PAUSE : BlinkSchedulerState::WAITING;
  postSaccadeBlinkPending_ = false;
  blinkPhase_ = BlinkPhase::HALF_CLOSED;
  blinkFramesRemaining_ = 1;
  ++eyeBlinkEvents_;
  if (doubleBlink) ++eyeDoubleBlinkEvents_;
  if (longBlink_) ++eyeLongBlinkEvents_;
  Serial.printf("[EYES] blink=%s\n", longBlink_ ? "LONG" : doubleBlink ? "DOUBLE" : "NORMAL");
}

void NewoDisplay::queuePostSaccadeBlink(uint32_t now) {
  if (!autonomousGazeLargeShift_ ||
      blinkPhase_ != BlinkPhase::OPEN || winkActive_ || blinkSchedulerState_ != BlinkSchedulerState::WAITING ||
      random(100) >= 25) return;
  postSaccadeBlinkPending_ = true;
  nextBlinkMs_ = now + static_cast<uint32_t>(random(150, 351));
}

void NewoDisplay::chooseAutonomousGazeTarget() {
  const auto clampWeight = [](int16_t value, int16_t minimum, int16_t maximum) -> int16_t {
    if (value < minimum) return minimum;
    return value > maximum ? maximum : value;
  };
  const int16_t curiosityOffset = static_cast<int16_t>(curiosity_) - kCuriosityBaseline;
  const int16_t socialOffset = static_cast<int16_t>(social_) - 38;
  const int16_t centerWeight = clampWeight(34 + socialOffset / 4 - curiosityOffset / 6, 28, 38);
  const int16_t sideWeight = clampWeight(42 + curiosityOffset / 4 - socialOffset / 8, 38, 48);
  const int16_t upperWeight = clampWeight(18 + curiosityOffset / 6, 15, 22);
  const int16_t sideRangeX = energy_ < 55 ? 16 : energy_ > 80 ? 20 : 18;
  const int16_t upperRangeX = energy_ < 55 ? 12 : energy_ > 80 ? 16 : 14;

  const long choice = random(100);
  if (choice < centerWeight) {
    gazeTargetX_ = static_cast<int16_t>(random(-6, 7));
    gazeTargetY_ = static_cast<int16_t>(random(-4, 5));
  } else if (choice < centerWeight + sideWeight) {
    const int16_t side = random(0, 2) ? 1 : -1;
    gazeTargetX_ = static_cast<int16_t>(side * random(12, sideRangeX + 1));
    gazeTargetY_ = static_cast<int16_t>(random(-3, 4));
  } else if (choice < centerWeight + sideWeight + upperWeight) {
    gazeTargetX_ = static_cast<int16_t>(random(-upperRangeX, upperRangeX + 1));
    gazeTargetY_ = static_cast<int16_t>(random(-10, -3));
  } else {
    gazeTargetX_ = static_cast<int16_t>(random(-8, 9));
    gazeTargetY_ = static_cast<int16_t>(random(5, 10));
  }
  const int16_t deltaX = gazeTargetX_ - gazeX_;
  const int16_t deltaY = gazeTargetY_ - gazeY_;
  const int16_t magnitudeX = deltaX < 0 ? -deltaX : deltaX;
  const int16_t magnitudeY = deltaY < 0 ? -deltaY : deltaY;
  autonomousGazeLargeShift_ = magnitudeX >= 12 || magnitudeY >= 7;
  autonomousGazePhase_ = AutonomousGazePhase::MOVING;
}

void NewoDisplay::updateAutonomousIdleGaze(uint32_t now) {
  if (autonomousEpisode_ != AutonomousEpisode::WAITING) {
    if (autonomousGazePhase_ == AutonomousGazePhase::MOVING) {
      gazeX_ = easeAutonomousGaze(gazeX_, gazeTargetX_);
      gazeY_ = easeAutonomousGaze(gazeY_, gazeTargetY_);
      if (gazeX_ == gazeTargetX_ && gazeY_ == gazeTargetY_) {
        fixationUntilMs_ = now + autonomousEpisodeHoldMs_;
        autonomousGazePhase_ = AutonomousGazePhase::FIXATING;
      }
    }
    return;
  }

  switch (autonomousGazePhase_) {
    case AutonomousGazePhase::CHOOSE_TARGET:
      chooseAutonomousGazeTarget();
      return;
    case AutonomousGazePhase::MOVING:
      gazeX_ = easeAutonomousGaze(gazeX_, gazeTargetX_);
      gazeY_ = easeAutonomousGaze(gazeY_, gazeTargetY_);
      if (gazeX_ != gazeTargetX_ || gazeY_ != gazeTargetY_) return;
      queuePostSaccadeBlink(now);
      autonomousGazeLargeShift_ = false;
      {
        const uint32_t inactiveMs = now - lastInteractionMs_;
        const InactivityStage stage = inactiveMs >= kDrowsyInactivityMs ? InactivityStage::DROWSY
                                    : inactiveMs >= kRelaxedInactivityMs ? InactivityStage::RELAXED
                                                                          : InactivityStage::ACTIVE;
        const uint32_t baseFixationMs = stage == InactivityStage::DROWSY
            ? static_cast<uint32_t>(random(2'200, 4'201))
            : stage == InactivityStage::RELAXED
                ? static_cast<uint32_t>(random(1'500, 3'101))
                : static_cast<uint32_t>(random(1'000, 2'401));
        const uint32_t fixationMs = adjustAutonomousFixation(baseFixationMs);
        fixationUntilMs_ = now + fixationMs;
        microCorrectionPending_ = random(100) < (stage == InactivityStage::ACTIVE ? 28 : 18);
        const long microCorrectionEnd = static_cast<long>(fixationMs * 2 / 3);
        microCorrectionAtMs_ = microCorrectionPending_ && microCorrectionEnd > 300
            ? now + static_cast<uint32_t>(random(300, microCorrectionEnd))
            : 0;
        microCorrectionPending_ = microCorrectionAtMs_ != 0;
        recordGazeTarget(static_cast<uint16_t>(fixationMs));
        autonomousGazePhase_ = AutonomousGazePhase::FIXATING;
      }
      return;
    case AutonomousGazePhase::FIXATING:
      if (static_cast<int32_t>(now - fixationUntilMs_) >= 0) {
        autonomousGazePhase_ = AutonomousGazePhase::CHOOSE_TARGET;
        return;
      }
      if (!microCorrectionPending_ || static_cast<int32_t>(now - microCorrectionAtMs_) < 0) return;
      microCorrectionPending_ = false;
      {
        const int16_t shift = static_cast<int16_t>(random(1, 4));
        const int16_t direction = random(0, 2) ? 1 : -1;
        if (random(100) < 70) {
          const int16_t candidate = gazeTargetX_ + direction * shift;
          gazeTargetX_ = candidate >= -kAutonomousGazeHardX && candidate <= kAutonomousGazeHardX
              ? candidate : gazeTargetX_ - direction * shift;
        } else {
          const int16_t candidate = gazeTargetY_ + direction * shift;
          gazeTargetY_ = candidate >= -kAutonomousGazeHardY && candidate <= kAutonomousGazeHardY
              ? candidate : gazeTargetY_ - direction * shift;
        }
        autonomousGazePhase_ = AutonomousGazePhase::MICRO_CORRECTION;
      }
      return;
    case AutonomousGazePhase::MICRO_CORRECTION:
      gazeX_ = easeAutonomousGaze(gazeX_, gazeTargetX_);
      gazeY_ = easeAutonomousGaze(gazeY_, gazeTargetY_);
      if (gazeX_ != gazeTargetX_ || gazeY_ != gazeTargetY_) return;
      autonomousGazePhase_ = static_cast<int32_t>(now - fixationUntilMs_) >= 0
          ? AutonomousGazePhase::CHOOSE_TARGET
          : AutonomousGazePhase::FIXATING;
      return;
  }
}

void NewoDisplay::updateGaze(uint32_t now) {
  const NewoDisplayMode activeMode = effectiveMode(now);
  if (activeMode == NewoDisplayMode::IDLE && autoFaceEnabled_) {
    updateAutonomousIdleGaze(now);
    return;
  }
  if (static_cast<int32_t>(now - nextGazeMs_) >= 0) {
    switch (activeMode) {
      case NewoDisplayMode::IDLE: {
        // Fixed RoboEyes positions reuse the normal gaze state; no idle randomness.
        if (faceStyle_ >= NewoFaceStyle::LOOK_LEFT && faceStyle_ <= NewoFaceStyle::LOOK_DOWN_RIGHT) {
          gazeTargetX_ = (faceStyle_ == NewoFaceStyle::LOOK_LEFT || faceStyle_ == NewoFaceStyle::LOOK_UP_LEFT ||
                          faceStyle_ == NewoFaceStyle::LOOK_DOWN_LEFT) ? -14 :
                         (faceStyle_ == NewoFaceStyle::LOOK_RIGHT || faceStyle_ == NewoFaceStyle::LOOK_UP_RIGHT ||
                          faceStyle_ == NewoFaceStyle::LOOK_DOWN_RIGHT) ? 14 : 0;
          gazeTargetY_ = (faceStyle_ == NewoFaceStyle::LOOK_UP || faceStyle_ == NewoFaceStyle::LOOK_UP_LEFT ||
                          faceStyle_ == NewoFaceStyle::LOOK_UP_RIGHT) ? -8 :
                         (faceStyle_ == NewoFaceStyle::LOOK_DOWN || faceStyle_ == NewoFaceStyle::LOOK_DOWN_LEFT ||
                          faceStyle_ == NewoFaceStyle::LOOK_DOWN_RIGHT) ? 8 : 0;
          nextGazeMs_ = now + 1000;
          break;
        }
        int16_t rangeX = 14;
        int16_t rangeY = 6;
        uint16_t minDelay = 1'300;
        uint16_t maxDelay = 3'401;
        if (faceStyle_ == NewoFaceStyle::TIRED || faceStyle_ == NewoFaceStyle::SLEEPY) {
          rangeX = faceStyle_ == NewoFaceStyle::SLEEPY ? 3 : 8;
          rangeY = faceStyle_ == NewoFaceStyle::SLEEPY ? 1 : 3;
          minDelay = faceStyle_ == NewoFaceStyle::SLEEPY ? 3'500 : 2'000;
          maxDelay = faceStyle_ == NewoFaceStyle::SLEEPY ? 6'001 : 4'201;
        } else if (faceStyle_ == NewoFaceStyle::CURIOUS) {
          rangeX = 17;
          rangeY = 7;
          minDelay = 900;
          maxDelay = 2'201;
        } else if (faceStyle_ == NewoFaceStyle::CONFUSED) {
          // Neutral eyes leave 29 px per side. Keep horizontal gaze centered so
          // the upstream ±20 px flicker remains wholly inside the 200 px canvas.
          rangeX = 0;
          rangeY = 3;
          minDelay = 1'400;
          maxDelay = 2'801;
        } else if (faceStyle_ == NewoFaceStyle::HAPPY || faceStyle_ == NewoFaceStyle::LAUGH) {
          rangeX = 10;
          rangeY = 4;
          minDelay = 1'000;
          maxDelay = 2'601;
        }
        gazeTargetX_ = static_cast<int16_t>(random(-rangeX, rangeX + 1));
        gazeTargetY_ = static_cast<int16_t>(random(-rangeY, rangeY + 1));
        nextGazeMs_ = now + static_cast<uint32_t>(random(minDelay, maxDelay));
        break;
      }
      case NewoDisplayMode::LISTENING:
        gazeTargetX_ = static_cast<int16_t>(random(-5, 6));
        gazeTargetY_ = static_cast<int16_t>(random(-3, 4));
        nextGazeMs_ = now + static_cast<uint32_t>(random(900, 1'601));
        break;
      case NewoDisplayMode::THINKING:
        gazeTargetX_ = random(0, 2) ? static_cast<int16_t>(random(8, 17))
                                    : static_cast<int16_t>(random(-16, -7));
        gazeTargetY_ = static_cast<int16_t>(random(-9, -3));
        nextGazeMs_ = now + static_cast<uint32_t>(random(1'100, 2'401));
        break;
      case NewoDisplayMode::SPEAKING:
        gazeTargetX_ = static_cast<int16_t>(random(-8, 9));
        gazeTargetY_ = static_cast<int16_t>(random(-2, 4));
        nextGazeMs_ = now + static_cast<uint32_t>(random(650, 1'301));
        break;
      default:
        gazeTargetX_ = 0;
        gazeTargetY_ = 0;
        nextGazeMs_ = now + 1'000;
        break;
    }
  }

  auto easeToward = [](int16_t current, int16_t target) -> int16_t {
    const int16_t delta = target - current;
    if (delta > -2 && delta < 2) return target;
    int16_t step = delta / 3;
    if (step == 0) step = delta > 0 ? 1 : -1;
    return current + step;
  };
  gazeX_ = easeToward(gazeX_, gazeTargetX_);
  gazeY_ = easeToward(gazeY_, gazeTargetY_);
}

void NewoDisplay::applyEyeExpression(int16_t leftX, int16_t rightX, int16_t y, int16_t leftW, int16_t rightW,
                                      int16_t height, NewoDisplayMode mode) {
  if (height < 8) return;

  bool tired = mode == NewoDisplayMode::THINKING;
  bool happy = mode == NewoDisplayMode::SPEAKING;
  bool angry = mode == NewoDisplayMode::ERROR;
  if (mode == NewoDisplayMode::IDLE) {
    tired = faceStyle_ == NewoFaceStyle::TIRED || faceStyle_ == NewoFaceStyle::SLEEPY ||
            (autoFaceEnabled_ && autonomousEpisode_ == AutonomousEpisode::LOW_ENERGY);
    happy = faceStyle_ == NewoFaceStyle::HAPPY || faceStyle_ == NewoFaceStyle::LAUGH ||
            (autoFaceEnabled_ && autonomousEpisode_ == AutonomousEpisode::SOCIAL_ATTENTION);
    angry = faceStyle_ == NewoFaceStyle::ANGRY;
  }

  if (tired) {
    const int16_t cover = height / 4;
    eyeCanvas_.fillTriangle(leftX, y, leftX + leftW, y, leftX, y + cover, 0);
    eyeCanvas_.fillTriangle(rightX, y, rightX + rightW, y, rightX + rightW, y + cover, 0);
  } else if (happy) {
    const int16_t leftCenter = leftX + leftW / 2;
    const int16_t rightCenter = rightX + rightW / 2;
    const int16_t radius = height / 2 + 5;
    const int16_t centerY = y + height + 4;
    eyeCanvas_.fillCircle(leftCenter, centerY, radius, 0);
    eyeCanvas_.fillCircle(rightCenter, centerY, radius, 0);
  } else if (angry) {
    const int16_t cover = height / 2;
    eyeCanvas_.fillTriangle(leftX, y, leftX + leftW, y, leftX + leftW, y + cover, 0);
    eyeCanvas_.fillTriangle(rightX, y, rightX + rightW, y, rightX, y + cover, 0);
  }
}

void NewoDisplay::drawFaceFrame(uint32_t now) {
  const uint32_t frameStartedUs = micros();
  eyeCanvas_.fillScreen(0);
  const NewoDisplayMode activeMode = effectiveMode(now);

  const bool winkStyle = activeMode == NewoDisplayMode::IDLE &&
                         (faceStyle_ == NewoFaceStyle::WINK_LEFT || faceStyle_ == NewoFaceStyle::WINK_RIGHT);
  if (winkActive_ && now - winkStartedMs_ >= kWinkBurstMs) {
    winkActive_ = false;
    nextWinkMs_ = now + static_cast<uint32_t>(random(kWinkCalmMinMs, kWinkCalmMaxMs));
    // Do not start a delayed autoblink immediately after a wink burst.
    nextBlinkMs_ = now + static_cast<uint32_t>(random(2'500, 5'501));
  }
  updateAutonomousEpisode(now);
  const bool deliberatelyClosed = activeMode == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::CLOSED;
  const bool autonomousEpisodeActive = autonomousEpisode_ != AutonomousEpisode::WAITING;
  if (blinkPhase_ == BlinkPhase::OPEN && !winkActive_) {
    if (autonomousEpisode_ == AutonomousEpisode::LOW_ENERGY && autonomousEpisodeBlinkRequested_ &&
        !autonomousEpisodeBlinkStarted_) {
      startBilateralBlink(false, kForceLongBlink);
      autonomousEpisodeBlinkStarted_ = true;
    } else if (!autonomousEpisodeActive && winkStyle && static_cast<int32_t>(now - nextWinkMs_) >= 0) {
      // A wink wins over and cancels any pending bilateral sequence.
      blinkSchedulerState_ = BlinkSchedulerState::WAITING;
      longBlink_ = false;
      postSaccadeBlinkPending_ = false;
      winkActive_ = true;
      winkLeft_ = faceStyle_ == NewoFaceStyle::WINK_LEFT;
      winkStartedMs_ = now;
    } else if (!autonomousEpisodeActive && !deliberatelyClosed && static_cast<int32_t>(now - nextBlinkMs_) >= 0) {
      // One scheduler owns bilateral events; episodes only request this existing scheduler.
      const bool doubleSecond = blinkSchedulerState_ == BlinkSchedulerState::DOUBLE_SECOND;
      startBilateralBlink(activeMode == NewoDisplayMode::IDLE && autoFaceEnabled_ &&
                              !postSaccadeBlinkPending_ && !doubleSecond);
    }
  }
  updateGaze(now);

  const float phase = static_cast<float>(now % 3000) / 3000.0f * 6.2831853f;
  const int16_t floatY = static_cast<int16_t>(sinf(phase) * (speakerActive_ ? 2.0f : 1.0f));

  int16_t leftW = 60;
  int16_t rightW = 60;
  int16_t baseHeight = 36;
  int16_t gap = 22;
  int16_t verticalOffset = 0;
  bool cyclops = false;

  // Autonomous expressions are temporary geometry overlays owned by the active
  // episode. They never mutate faceStyle_, so manual /face selections remain
  // persistent and operational contexts still preempt immediately.
  if (activeMode == NewoDisplayMode::IDLE && autoFaceEnabled_) {
    const uint32_t inactiveMs = now - lastInteractionMs_;
    switch (autonomousEpisode_) {
      case AutonomousEpisode::CURIOUS_SCAN:
        leftW = rightW = 59;
        baseHeight = 38;
        gap = 22;
        break;
      case AutonomousEpisode::LOW_ENERGY:
        leftW = rightW = 62;
        gap = 22;
        if (inactiveMs >= kDrowsyInactivityMs) {
          baseHeight = 20;
          verticalOffset = 5;
        } else {
          baseHeight = 32;
          verticalOffset = 3;
        }
        break;
      case AutonomousEpisode::SOCIAL_ATTENTION:
        leftW = rightW = 64;
        baseHeight = 39;
        gap = 20;
        break;
      case AutonomousEpisode::ALERT_CHECK:
        if (autonomousEpisodeDirection_ > 0) {
          leftW = rightW = 54;
          baseHeight = 54;
          gap = 28;
        } else {
          leftW = 55;
          rightW = 65;
          baseHeight = 36;
          gap = 22;
        }
        break;
      case AutonomousEpisode::WAITING:
        break;
    }
  }

  if (activeMode == NewoDisplayMode::IDLE) {
    switch (faceStyle_) {
      case NewoFaceStyle::HAPPY:
        leftW = rightW = 64;
        baseHeight = 39;
        gap = 20;
        break;
      case NewoFaceStyle::ANGRY:
        leftW = rightW = 62;
        baseHeight = 35;
        gap = 20;
        verticalOffset = 3;
        break;
      case NewoFaceStyle::TIRED:
        leftW = rightW = 62;
        baseHeight = 32;
        gap = 22;
        verticalOffset = 3;
        break;
      case NewoFaceStyle::CURIOUS:
        leftW = rightW = 59;
        baseHeight = 38;
        gap = 22;
        break;
      case NewoFaceStyle::CONFUSED:
        // Keep neutral geometry: the upstream horizontal flicker is the expression.
        break;
      case NewoFaceStyle::LAUGH:
        leftW = rightW = 67;
        baseHeight = 33;
        gap = 18;
        verticalOffset = 2;
        break;
      case NewoFaceStyle::SWEAT:
        leftW = 61;
        rightW = 55;
        baseHeight = 36;
        gap = 23;
        break;
      case NewoFaceStyle::CLOSED:
        baseHeight = 4;
        break;
      case NewoFaceStyle::SURPRISED:
        leftW = rightW = 54; baseHeight = 54; gap = 28;
        break;
      case NewoFaceStyle::SLEEPY:
        leftW = rightW = 62; baseHeight = 20; verticalOffset = 5;
        break;
      case NewoFaceStyle::CYCLOPS:
        leftW = 78;
        rightW = 0;
        baseHeight = 43;
        gap = 0;
        cyclops = true;
        break;
      default:
        break;
    }
  } else {
    switch (activeMode) {
      case NewoDisplayMode::LISTENING:
        leftW = rightW = 69;
        baseHeight = 42;
        gap = 17;
        break;
      case NewoDisplayMode::THINKING:
        leftW = 56;
        rightW = 62;
        baseHeight = 35;
        gap = 22;
        verticalOffset = -2;
        break;
      case NewoDisplayMode::SPEAKING:
        leftW = rightW = 62;
        baseHeight = 38;
        gap = 20;
        break;
      case NewoDisplayMode::ERROR:
        leftW = rightW = 62;
        baseHeight = 34;
        gap = 20;
        verticalOffset = 5;
        break;
      default:
        break;
    }
  }

  int16_t curiousLeftLift = 0;
  int16_t curiousRightLift = 0;
  const bool autonomousCuriosity = activeMode == NewoDisplayMode::IDLE && autoFaceEnabled_ &&
                                    autonomousEpisode_ == AutonomousEpisode::CURIOUS_SCAN;
  if (activeMode == NewoDisplayMode::IDLE && (faceStyle_ == NewoFaceStyle::CURIOUS || autonomousCuriosity)) {
    // During an autonomous curious scan use the full readable curiosity lift;
    // a 4 px lift from V2 was too subtle on the physical 240 px display.
    const int16_t lift = 8;
    if (gazeX_ < -7 || (autonomousCuriosity && gazeTargetX_ < 0)) curiousLeftLift = lift;
    if (gazeX_ > 7 || (autonomousCuriosity && gazeTargetX_ > 0)) curiousRightLift = lift;
  } else if (activeMode == NewoDisplayMode::THINKING) {
    if (gazeX_ < -7) leftW += 7;
    if (gazeX_ > 7) rightW += 7;
  }

  int16_t shake = 0;
  if (activeMode == NewoDisplayMode::ERROR && now - modeStartedMs_ < 520) {
    shake = static_cast<int16_t>(sinf(static_cast<float>(now - modeStartedMs_) * 0.075f) * 4.0f);
  }
  if (activeMode == NewoDisplayMode::IDLE && autoFaceEnabled_ &&
      autonomousEpisode_ == AutonomousEpisode::ALERT_CHECK && autonomousEpisodeDirection_ < 0) {
    // The confused variant stays visibly asymmetric but bounded; unlike the
    // manual confused face it does not use a ±20 px shake on top of ±20 gaze.
    shake += static_cast<int16_t>(sinf(static_cast<float>(now) * 0.055f) * 6.0f);
  }
  const uint32_t styleBurstMs = (now - modeStartedMs_) % 2200; // selection-relative 500 ms burst, then calm.
  if (activeMode == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::CONFUSED && styleBurstMs < 500) {
    // Upstream anim_confused: approximately 20 px horizontal flicker.
    shake += static_cast<int16_t>(sinf(static_cast<float>(styleBurstMs) * 0.075f) * 20.0f);
  }
  if (activeMode == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::LAUGH && styleBurstMs < 500) {
    // Upstream anim_laugh: approximately 5 px vertical flicker; HAPPY rests between bursts.
    verticalOffset += static_cast<int16_t>(sinf(static_cast<float>(styleBurstMs) * 0.075f) * 5.0f);
  }

  int16_t height = baseHeight;
  if (blinkPhase_ == BlinkPhase::HALF_CLOSED || blinkPhase_ == BlinkPhase::HALF_OPEN) {
    height = static_cast<int16_t>(baseHeight * 0.40f);
  } else if (blinkPhase_ == BlinkPhase::CLOSED) {
    height = static_cast<int16_t>(baseHeight * 0.07f);
    if (height < 2) height = 2;
  }
  if (cyclops) {
    const int16_t x = (kEyeCanvasWidth - leftW) / 2 + gazeX_ + shake;
    int16_t y = (kEyeCanvasHeight - baseHeight) / 2 + gazeY_ + floatY + verticalOffset;
    y += (baseHeight - height) / 2;
    const int16_t radius = height > 3 ? height / 2 : 1;
    eyeCanvas_.fillRoundRect(x, y, leftW, height, radius, 1);
  } else {
    const int16_t totalWidth = leftW + gap + rightW;
    int16_t leftX = (kEyeCanvasWidth - totalWidth) / 2 + gazeX_ + shake;
    int16_t rightX = leftX + leftW + gap;
    int16_t y = (kEyeCanvasHeight - baseHeight) / 2 + gazeY_ + floatY + verticalOffset;
    y += (baseHeight - height) / 2;
    const int16_t radius = height > 3 ? height / 2 : 1;

    int16_t leftHeight = height;
    int16_t rightHeight = height;
    int16_t leftY = y;
    int16_t rightY = y;
    if (winkActive_) {
      const uint32_t winkElapsedMs = now - winkStartedMs_;
      int16_t winkHeight = baseHeight;
      if (winkElapsedMs < 60 || winkElapsedMs >= 180) winkHeight = static_cast<int16_t>(baseHeight * 0.40f);
      else winkHeight = static_cast<int16_t>(baseHeight * 0.07f);
      if (winkHeight < 2) winkHeight = 2;
      if (winkLeft_) {
        leftHeight = winkHeight;
        leftY += (baseHeight - winkHeight) / 2;
      } else {
        rightHeight = winkHeight;
        rightY += (baseHeight - winkHeight) / 2;
      }
    }
    if (height >= 8 && curiousLeftLift) {
      leftHeight += curiousLeftLift;
      leftY -= curiousLeftLift / 2;
    }
    if (height >= 8 && curiousRightLift) {
      rightHeight += curiousRightLift;
      rightY -= curiousRightLift / 2;
    }

    eyeCanvas_.fillRoundRect(leftX, leftY, leftW, leftHeight, leftHeight > 3 ? leftHeight / 2 : 1, 1);
    eyeCanvas_.fillRoundRect(rightX, rightY, rightW, rightHeight, rightHeight > 3 ? rightHeight / 2 : 1, 1);
    applyEyeExpression(leftX, rightX, y, leftW, rightW, height, activeMode);

    if (activeMode == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::SWEAT && height >= 8) {
      // Three independent phase offsets across the forehead; each falls, grows, then shrinks.
      for (uint8_t drop = 0; drop < 3; ++drop) {
        const uint16_t phase = static_cast<uint16_t>((now / 9 + drop * 31) % 100);
        const int16_t dropX = 42 + drop * 58;
        const int16_t dropY = 3 + phase / 4;
        const int16_t radius = phase < 50 ? 1 + phase / 25 : 1 + (99 - phase) / 25;
        eyeCanvas_.fillCircle(dropX, dropY + radius, radius, 1);
        eyeCanvas_.fillTriangle(dropX, dropY - radius - 1, dropX - radius, dropY + radius, dropX + radius, dropY + radius, 1);
      }
    }
  }

  blitMonoCanvasFast(eyeCanvas_, kEyeCanvasX, kEyeCanvasY, kEyeCanvasWidth, kEyeCanvasHeight);

  if (blinkPhase_ != BlinkPhase::OPEN) {
    if (--blinkFramesRemaining_ == 0) {
      if (blinkPhase_ == BlinkPhase::HALF_CLOSED) {
        blinkPhase_ = BlinkPhase::CLOSED;
        blinkFramesRemaining_ = longBlink_ ? 3 : 1;
      } else if (blinkPhase_ == BlinkPhase::CLOSED) {
        blinkPhase_ = BlinkPhase::HALF_OPEN;
        blinkFramesRemaining_ = 1;
      } else {
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
    }
  }

  drawStateAnimation(now);
  recordFaceFrame(micros() - frameStartedUs);
}

void NewoDisplay::blitMonoCanvasFast(GFXcanvas1& canvas, int16_t x, int16_t y, int16_t width, int16_t height) {
  const uint8_t* source = canvas.getBuffer();
  const int16_t stride = (width + 7) / 8;
  display_.startWrite();
  display_.setAddrWindow(x, y, width, height);
  for (int16_t row = 0; row < height; ++row) {
    const uint8_t* scanline = source + row * stride;
    for (int16_t column = 0; column < width; ++column) {
      monoLineBuffer_[column] = (scanline[column >> 3] & (0x80 >> (column & 7))) ? kWhite : ST77XX_BLACK;
    }
    display_.writePixels(monoLineBuffer_, width);
  }
  display_.endWrite();
}

void NewoDisplay::recordFaceFrame(uint32_t elapsedUs) {
  // Wall time includes any preemption by the higher-priority speaker task; a
  // large value here does not imply that display SPI blocked audio for that long.
  if (!frameMetricsStartedMs_) frameMetricsStartedMs_ = millis();
  frameMetricsTotalUs_ += elapsedUs;
  if (elapsedUs > frameMetricsWorstUs_) frameMetricsWorstUs_ = elapsedUs;
  ++frameMetricsCount_;
  const uint32_t now = millis();
  if (now - frameMetricsStartedMs_ < 5'000) return;
  Serial.printf("DISPLAY_FRAME avg_us=%lu worst_us=%lu frames=%u\n",
                static_cast<unsigned long>(frameMetricsTotalUs_ / frameMetricsCount_),
                static_cast<unsigned long>(frameMetricsWorstUs_), static_cast<unsigned>(frameMetricsCount_));
  frameMetricsStartedMs_ = now;
  frameMetricsTotalUs_ = 0;
  frameMetricsWorstUs_ = 0;
  frameMetricsCount_ = 0;
}

void NewoDisplay::drawStateAnimation(uint32_t now) {
  activityCanvas_.fillScreen(0);
  const float phase = static_cast<float>(now % 2400) / 2400.0f * 6.2831853f;
  const NewoDisplayMode activeMode = effectiveMode(now);
  if (activeMode == NewoDisplayMode::LISTENING) {
    for (int8_t i = 0; i < 7; ++i) {
      const int16_t height = 5 + static_cast<int16_t>((sinf(phase * 2.0f + i * 0.8f) + 1.0f) * 5.5f);
      activityCanvas_.fillRect(12 + i * 12, 16 - height, 5, height, 1);
    }
  } else if (activeMode == NewoDisplayMode::THINKING) {
    for (int8_t i = 0; i < 3; ++i) {
      const int16_t rise = static_cast<int16_t>((sinf(phase * 1.5f + i * 1.4f) + 1.0f) * 3.0f);
      activityCanvas_.fillCircle(36 + i * 12, 15 - rise, 2, 1);
    }
  } else if (activeMode == NewoDisplayMode::SPEAKING) {
    int16_t lastY = 172;
    for (int16_t x = 0; x <= 72; x += 4) {
      const int16_t y = 172 + static_cast<int16_t>(sinf(phase * 2.0f + x * 0.16f) * 6.0f);
      activityCanvas_.drawLine(x - 4, lastY - 160, x, y - 160, 1);
      lastY = y;
    }
  } else if (activeMode == NewoDisplayMode::ERROR) {
    activityCanvas_.setFont(&FreeSans9pt7b);
    activityCanvas_.setCursor(44, 17);
    activityCanvas_.print("!");
    activityCanvas_.setFont(nullptr);
  }
  blitMonoCanvasFast(activityCanvas_, 72, 160, 96, 23);
}

void NewoDisplay::drawFaceResponse() {
  if (!text_[0]) return;
  char response[49] = {};
  strncpy(response, text_, sizeof(response) - 1);
  if (strlen(text_) >= sizeof(response)) {
    response[44] = '.';
    response[45] = '.';
    response[46] = '.';
    response[47] = '\0';
  }
  drawWrapped(response, 204, false, false, 2);
}

void NewoDisplay::drawTextPage(const char* heading, const char* body, bool info) {
  constexpr int16_t kMargin = 16;
  char inferredHeading[25] = {};
  if (info && (!heading || !heading[0]) && body) {
    const char* newline = strchr(body, '\n');
    const size_t length = newline ? static_cast<size_t>(newline - body) : strlen(body);
    if (length > 0 && length < sizeof(inferredHeading)) {
      memcpy(inferredHeading, body, length);
      heading = inferredHeading;
      body = newline ? newline + 1 : "";
    }
  }
  int16_t firstY = 28;
  if (heading && heading[0]) {
    display_.setFont(&FreeSansBold9pt7b);
    display_.setCursor(kMargin, 24);
    display_.print(heading);
    display_.setFont(nullptr);
    firstY = 48;
  }
  drawWrapped(body, firstY, info, false);
}

void NewoDisplay::drawMessage() {
  uint8_t words = 0;
  bool inWord = false;
  for (const char* cursor = text_; *cursor; ++cursor) {
    if (*cursor == '\n') {
      words = 3;
      break;
    }
    if (*cursor != ' ' && !inWord) {
      ++words;
      inWord = true;
    }
    if (*cursor == ' ') inWord = false;
  }
  const bool shortMessage = words > 0 && words <= 2 && strlen(text_) <= 14;
  if (shortMessage) {
    display_.setFont(&FreeSans18pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    display_.getTextBounds(text_, 0, 0, &x1, &y1, &w, &h);
    display_.setCursor((kWidth - w) / 2, 132);
    display_.print(text_);
    display_.setFont(nullptr);
    return;
  }
  drawTextPage("", text_, false);
}

void NewoDisplay::drawEco() {
  char body[160];
  char up[20];
  formatUptime(up, sizeof(up), telemetry_.uptimeMs);
  if (ecoPage_ == 0) {
    snprintf(body, sizeof(body), "Online  %s\nWiFi    %s\nRSSI    %ld dBm\nUp      %s\nFW      %s",
             telemetry_.cloud ? "YES" : "NO", telemetry_.wifi ? "OK" : "OFF",
             static_cast<long>(telemetry_.rssi), up, NewoConfig::FIRMWARE_VERSION);
    drawTextPage("NEWO", body, true);
  } else if (ecoPage_ == 1) {
    snprintf(body, sizeof(body), "Heap    %luK\nPSRAM   %luK\nWarn    %lu\nError   %lu",
             static_cast<unsigned long>(telemetry_.heap / 1024),
             static_cast<unsigned long>(telemetry_.psram / 1024),
             static_cast<unsigned long>(telemetry_.logs.warnings),
             static_cast<unsigned long>(telemetry_.logs.errors));
    drawTextPage("HEALTH", body, true);
  } else {
    snprintf(body, sizeof(body), "WiFi    %s\nCloud   %s\nWarn    %lu\nError   %lu",
             telemetry_.wifi ? "OK" : "OFF", telemetry_.cloud ? "OK" : "OFF",
             static_cast<unsigned long>(telemetry_.logs.warnings),
             static_cast<unsigned long>(telemetry_.logs.errors));
    drawTextPage("SERVICES", body, true);
  }
}

void NewoDisplay::drawCentered(const char* text, int16_t y) {
  display_.setFont(&FreeSans9pt7b);
  int16_t x1, y1;
  uint16_t w, h;
  display_.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display_.setCursor((kWidth - w) / 2, y);
  display_.print(text);
  display_.setFont(nullptr);
}

void NewoDisplay::drawWrapped(const char* text, int16_t firstY, bool info, bool centered, uint8_t maxLines) {
  constexpr int16_t kMargin = 16;
  const int16_t maxWidth = kWidth - (kMargin * 2);
  const int16_t lineHeight = info ? 17 : 19;
  display_.setFont(info ? &FreeMono9pt7b : &FreeSans9pt7b);
  char line[97] = {};
  size_t length = 0;
  int16_t y = firstY;
  uint8_t lines = 0;
  const char* cursor = text ? text : "";
  auto flush = [&]() {
    if (length == 0 || y > 230) return;
    line[length] = '\0';
    if (centered)
      drawCentered(line, y);
    else {
      display_.setCursor(kMargin, y);
      display_.print(line);
    }
    display_.setFont(info ? &FreeMono9pt7b : &FreeSans9pt7b);
    y += lineHeight;
    ++lines;
    length = 0;
  };
  while (*cursor && y <= 230 && (!maxLines || lines < maxLines)) {
    if (*cursor == '\n') {
      flush();
      ++cursor;
      continue;
    }
    while (*cursor == ' ') ++cursor;
    if (!*cursor) break;
    if (*cursor == '\n') {
      flush();
      ++cursor;
      continue;
    }
    char word[97] = {};
    size_t wordLength = 0;
    while (cursor[wordLength] && cursor[wordLength] != ' ' && cursor[wordLength] != '\n' &&
           wordLength < sizeof(word) - 1) {
      word[wordLength] = cursor[wordLength];
      ++wordLength;
    }
    word[wordLength] = '\0';
    cursor += wordLength;
    char candidate[97] = {};
    snprintf(candidate, sizeof(candidate), "%s%s%s", line, length ? " " : "", word);
    int16_t x1, y1;
    uint16_t width, height;
    display_.getTextBounds(candidate, 0, 0, &x1, &y1, &width, &height);
    if (width <= maxWidth) {
      strncpy(line, candidate, sizeof(line) - 1);
      length = strlen(line);
      continue;
    }
    if (length) flush();
    for (size_t index = 0; index < wordLength && y <= 230; ++index) {
      line[length++] = word[index];
      line[length] = '\0';
      display_.getTextBounds(line, 0, 0, &x1, &y1, &width, &height);
      if (width > maxWidth) {
        line[--length] = '\0';
        flush();
        line[length++] = word[index];
        line[length] = '\0';
      }
    }
  }
  flush();
  display_.setFont(nullptr);
}
