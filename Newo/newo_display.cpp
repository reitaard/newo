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
constexpr uint32_t kNormalFaceFrameMs = 50;
constexpr uint32_t kSpeakingFaceFrameMs = 120;
constexpr uint32_t kWinkCalmMinMs = 2'000;
constexpr uint32_t kWinkCalmMaxMs = 4'001;
constexpr uint16_t kWhite = ST77XX_WHITE;
constexpr int16_t kEyeCanvasWidth = 200;
constexpr int16_t kEyeCanvasHeight = 82;
constexpr int16_t kEyeCanvasX = 20;
constexpr int16_t kEyeCanvasY = 40;
constexpr uint32_t kAutonomousStateUpdateMs = 3'000;
constexpr uint32_t kAutonomousStateLogMs = 60'000;
constexpr uint32_t kAutonomousEpisodeMinMs = 8'000;
constexpr uint32_t kAutonomousEpisodeMaxMs = 18'000;
constexpr int16_t kAutonomousGazeHardX = 20;
constexpr int16_t kAutonomousGazeHardY = 12;
constexpr uint8_t kForceDoubleBlink = 1;
constexpr uint8_t kForceLongBlink = 2;

NewoAutonomyEngagement autonomyEngagementFor(NewoDisplayMode mode) {
  switch (mode) {
    case NewoDisplayMode::LISTENING: return NewoAutonomyEngagement::LISTENING;
    case NewoDisplayMode::THINKING: return NewoAutonomyEngagement::THINKING;
    case NewoDisplayMode::SPEAKING: return NewoAutonomyEngagement::SPEAKING;
    default: return NewoAutonomyEngagement::IDLE;
  }
}

const char* inactivityStageName(NewoInactivityStage stage) {
  switch (stage) {
    case NewoInactivityStage::ACTIVE: return "ACTIVE";
    case NewoInactivityStage::RELAXED: return "RELAXED";
    case NewoInactivityStage::DROWSY: return "DROWSY";
  }
  return "UNKNOWN";
}

uint8_t clampPercent(int value) {
  if (value < 0) return 0;
  return value > 100 ? 100 : static_cast<uint8_t>(value);
}

void formatUptime(char* out, size_t size, uint32_t ms) {
  const uint32_t seconds = ms / 1000;
  snprintf(out, size, "%luh %02lum", static_cast<unsigned long>(seconds / 3600),
           static_cast<unsigned long>((seconds / 60) % 60));
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
}

bool NewoDisplay::setFaceStyle(NewoFaceStyle style) {
  if (style > NewoFaceStyle::SKEPTICAL) return false;
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
    case AutonomousEpisode::DROWSY_REST: return "DROWSY_REST";
    case AutonomousEpisode::WAITING: return "WAITING";
  }
  return "UNKNOWN";
}

const char* NewoDisplay::expressionName(AutonomousExpression expression) {
  switch (expression) {
    case AutonomousExpression::CURIOUS: return "CURIOUS";
    case AutonomousExpression::HAPPY: return "HAPPY";
    case AutonomousExpression::TIRED: return "TIRED";
    case AutonomousExpression::SLEEPY: return "SLEEPY";
    case AutonomousExpression::SURPRISED: return "SURPRISED";
    case AutonomousExpression::CONFUSED: return "CONFUSED";
    case AutonomousExpression::SLEEPING: return "SLEEPING";
    case AutonomousExpression::NONE: return "NONE";
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
  const NewoInactivityStage stage = autonomyState_.stage(now);
  Serial.printf("[EYES_STATS] context=%s context_changes=%lu energy=%u fatigue=%u curiosity=%u social=%u stress=%u stage=%s "
                "gaze=%lu meaningful_gaze=%lu blinks=%lu double_blinks=%lu long_blinks=%lu "
                "episodes=%lu completed=%lu errors=%lu\n",
                contextName(effectiveMode(now)), static_cast<unsigned long>(eyeContextChanges_),
                static_cast<unsigned>(autonomyState_.energy()), static_cast<unsigned>(autonomyState_.fatigue()),
                static_cast<unsigned>(autonomyState_.curiosity()), static_cast<unsigned>(autonomyState_.social()),
                static_cast<unsigned>(autonomyState_.stress()), inactivityStageName(stage),
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
  if (autonomousIdle()) scheduleNextBilateralBlink(now);
  else nextBlinkMs_ = now + 1'000;
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
  gazeMotion_.reset(0, 0);
  nextGazeMs_ = now;
  nextFaceFrameMs_ = 0;
  resetAutonomousEpisode(now);
}

NewoDisplayMode NewoDisplay::effectiveMode(uint32_t now) const {
  if (ecoEnabled_ || mode_ == NewoDisplayMode::ECO) return NewoDisplayMode::ECO;
  if (temporary_ || mode_ == NewoDisplayMode::MESSAGE) return NewoDisplayMode::MESSAGE;
  if (mode_ == NewoDisplayMode::ERROR ||
      (errorActive_ && static_cast<int32_t>(now - errorUntilMs_) < 0)) return NewoDisplayMode::ERROR;
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
    case NewoDisplayMode::LISTENING: noteInteraction(now, 5, 10, 12); break;
    case NewoDisplayMode::THINKING: noteInteraction(now, 2, 4, 5); break;
    case NewoDisplayMode::SPEAKING: noteInteraction(now, 4, 5, 10); break;
    default: break;
  }
  resetFaceMotion(now);
  dirty_ = true;
}

void NewoDisplay::initializeAutonomousState(uint32_t now) {
  autonomyState_.reset(now);
  nextAutonomousStateMs_ = now + kAutonomousStateUpdateMs;
  nextAutonomousStateLogMs_ = now + kAutonomousStateLogMs;
}

void NewoDisplay::noteInteraction(uint32_t now, uint8_t energyGain, uint8_t curiosityGain, uint8_t socialGain) {
  autonomyState_.noteInteraction(now, energyGain, curiosityGain, socialGain);
}

void NewoDisplay::noteError() {
  ++eyeErrorEvents_;
  autonomyState_.noteError();
}

void NewoDisplay::setAutonomousExpression(AutonomousExpression expression, uint8_t intensity) {
  if (intensity > 100) intensity = 100;
  if (expression == autonomousExpression_ && intensity == autonomousExpressionIntensity_) return;
  autonomousExpression_ = expression;
  autonomousExpressionIntensity_ = expression == AutonomousExpression::NONE ? 0 : intensity;
  Serial.printf("[EYES] expr=%s intensity=%u\n", expressionName(autonomousExpression_),
                static_cast<unsigned>(autonomousExpressionIntensity_));
}

void NewoDisplay::clearAutonomousExpression() {
  setAutonomousExpression(AutonomousExpression::NONE, 0);
}

void NewoDisplay::updateAutonomousState(uint32_t now) {
  if (static_cast<int32_t>(now - nextAutonomousStateMs_) >= 0) {
    nextAutonomousStateMs_ = now + kAutonomousStateUpdateMs;
    autonomyState_.update(now, autonomyEngagementFor(effectiveMode(now)));
  }
  maybeLogEyeStats(now);
}

void NewoDisplay::resetAutonomousEpisode(uint32_t now) {
  autonomousEpisode_ = AutonomousEpisode::WAITING;
  clearAutonomousExpression();
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
  const uint32_t inactiveMs = autonomyState_.inactiveMs(now);
  if (inactiveMs >= NewoAutonomyState::kDrowsyInactivityMs) {
    minimumMs += 10'000;
    maximumMs += 14'000;
  } else if (inactiveMs >= NewoAutonomyState::kRelaxedInactivityMs) {
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
  clearAutonomousExpression();
  autonomousEpisodeHoldMs_ = 0;
  autonomousEpisodeStep_ = 0;
  autonomousEpisodeBlinkRequested_ = false;
  autonomousEpisodeBlinkStarted_ = false;
  if (blinkPhase_ == BlinkPhase::OPEN && blinkSchedulerState_ == BlinkSchedulerState::WAITING &&
      static_cast<int32_t>(now - nextBlinkMs_) >= 0) scheduleNextBilateralBlink(now);
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
  gazeMotion_.start(gazeX_, gazeY_, gazeTargetX_, gazeTargetY_,
                    kAutonomousGazeHardX, kAutonomousGazeHardY, true);
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
  const NewoInactivityStage stage = autonomyState_.stage(now);
  const uint8_t energy = autonomyState_.energy();
  const uint8_t curiosity = autonomyState_.curiosity();
  const uint8_t social = autonomyState_.social();
  const uint8_t stress = autonomyState_.stress();
  int curiousWeight = 30 + (curiosity > NewoAutonomyState::kCuriosityBaseline
      ? static_cast<int>(curiosity - NewoAutonomyState::kCuriosityBaseline) / 2 : 0);
  int socialWeight = 24 + static_cast<int>(social) / 10;
  int alertWeight = 20 + static_cast<int>(stress);
  int lowEnergyWeight = 8 + (energy < 70 ? static_cast<int>(70 - energy) : 0);
  int restWeight = 0;
  if (curiosity > 55) curiousWeight += 10;
  if (social > 60) socialWeight += 10;
  if (stress > 8) alertWeight += 20;
  if (energy < 55) lowEnergyWeight += 15;
  if (stage == NewoInactivityStage::RELAXED) {
    if (curiousWeight > 5) curiousWeight -= 5;
    socialWeight += 3;
    lowEnergyWeight += 15;
  } else if (stage == NewoInactivityStage::DROWSY) {
    if (curiousWeight > 10) curiousWeight -= 10;
    if (socialWeight > 5) socialWeight -= 5;
    if (alertWeight > 5) alertWeight -= 5;
    lowEnergyWeight += 35;
    restWeight = 10 + (energy < 55 ? static_cast<int>(55 - energy) / 2 : 0);
  }
  const int totalWeight = curiousWeight + socialWeight + alertWeight + lowEnergyWeight + restWeight;
  const long choice = random(totalWeight);
  int threshold = curiousWeight;
  if (choice < threshold) autonomousEpisode_ = AutonomousEpisode::CURIOUS_SCAN;
  else if (choice < (threshold += socialWeight)) autonomousEpisode_ = AutonomousEpisode::SOCIAL_ATTENTION;
  else if (choice < (threshold += alertWeight)) autonomousEpisode_ = AutonomousEpisode::ALERT_CHECK;
  else if (choice < (threshold += lowEnergyWeight)) autonomousEpisode_ = AutonomousEpisode::LOW_ENERGY;
  else autonomousEpisode_ = AutonomousEpisode::DROWSY_REST;
  autonomousEpisodeStep_ = 0;
  autonomousEpisodeDirection_ = random(0, 2) == 0 ? -1 : 1;
  autonomousEpisodeBlinkRequested_ = false;
  autonomousEpisodeBlinkStarted_ = false;
  ++eyeEpisodeStarts_;
  Serial.printf("[EYES] episode=%s start\n", episodeName(autonomousEpisode_));

  switch (autonomousEpisode_) {
    case AutonomousEpisode::CURIOUS_SCAN: {
      const int curiosityDelta = curiosity > NewoAutonomyState::kCuriosityBaseline
          ? static_cast<int>(curiosity - NewoAutonomyState::kCuriosityBaseline) : 0;
      setAutonomousExpression(AutonomousExpression::CURIOUS, clampPercent(72 + curiosityDelta * 3));
      beginAutonomousEpisodeGaze(now, static_cast<int16_t>(autonomousEpisodeDirection_ * random(16, 21)),
                                 static_cast<int16_t>(random(-4, 5)), static_cast<uint16_t>(random(800, 1'301)));
      break;
    }
    case AutonomousEpisode::LOW_ENERGY: {
      const int lowEnergy = energy < 70 ? static_cast<int>(70 - energy) : 0;
      const bool drowsy = stage == NewoInactivityStage::DROWSY;
      setAutonomousExpression(drowsy ? AutonomousExpression::SLEEPY : AutonomousExpression::TIRED,
                              drowsy ? clampPercent(88 + lowEnergy / 2) : clampPercent(65 + lowEnergy));
      beginAutonomousEpisodeGaze(now, static_cast<int16_t>(random(-4, 5)), static_cast<int16_t>(random(4, 12)),
                                 static_cast<uint16_t>(drowsy ? random(2'200, 3'801) : random(1'600, 2'801)));
      break;
    }
    case AutonomousEpisode::SOCIAL_ATTENTION:
      setAutonomousExpression(AutonomousExpression::HAPPY,
                              clampPercent(55 + static_cast<int>(social) / 2));
      beginAutonomousEpisodeGaze(now, static_cast<int16_t>(random(-3, 4)), static_cast<int16_t>(random(-2, 3)),
                                 static_cast<uint16_t>(random(1'600, 2'801)));
      break;
    case AutonomousEpisode::ALERT_CHECK:
      setAutonomousExpression(autonomousEpisodeDirection_ > 0 ? AutonomousExpression::SURPRISED
                                                              : AutonomousExpression::CONFUSED,
                              clampPercent(70 + static_cast<int>(stress) * 2));
      beginAutonomousEpisodeGaze(now, static_cast<int16_t>(autonomousEpisodeDirection_ * random(16, 21)),
                                 static_cast<int16_t>(random(-5, 4)), static_cast<uint16_t>(random(500, 901)));
      break;
    case AutonomousEpisode::DROWSY_REST:
      setAutonomousExpression(AutonomousExpression::SLEEPY, 88);
      beginAutonomousEpisodeGaze(now, static_cast<int16_t>(random(-3, 4)),
                                 static_cast<int16_t>(random(6, 10)),
                                 static_cast<uint16_t>(random(1'200, 1'801)));
      break;
    case AutonomousEpisode::WAITING: break;
  }
}

void NewoDisplay::advanceAutonomousEpisode(uint32_t now) {
  switch (autonomousEpisode_) {
    case AutonomousEpisode::CURIOUS_SCAN:
      if (autonomousEpisodeStep_ == 0) {
        autonomousEpisodeStep_ = 1;
        beginAutonomousEpisodeGaze(now, static_cast<int16_t>(autonomousEpisodeDirection_ * kAutonomousGazeHardX),
                                   static_cast<int16_t>(random(-5, 4)), 550);
      } else if (autonomousEpisodeStep_ == 1) {
        autonomousEpisodeStep_ = 2;
        beginAutonomousEpisodeGaze(now, 0, 0, 850);
      } else finishAutonomousEpisode(now);
      return;
    case AutonomousEpisode::LOW_ENERGY:
      if (autonomousEpisodeStep_ == 0) {
        autonomousEpisodeStep_ = 1;
        autonomousEpisodeBlinkRequested_ = true;
      } else if (autonomousEpisodeStep_ == 2) finishAutonomousEpisode(now);
      return;
    case AutonomousEpisode::DROWSY_REST:
      if (autonomousEpisodeStep_ == 0) {
        autonomousEpisodeStep_ = 1;
        autonomousEpisodeBlinkRequested_ = true;
      } else if (autonomousEpisodeStep_ == 2) {
        autonomousEpisodeStep_ = 3;
        setAutonomousExpression(AutonomousExpression::SLEEPY, 88);
        beginAutonomousEpisodeGaze(now, 0, 4, 900);
      } else if (autonomousEpisodeStep_ == 3) {
        autonomousEpisodeStep_ = 4;
        clearAutonomousExpression();
        beginAutonomousEpisodeGaze(now, 0, 0, 700);
      } else if (autonomousEpisodeStep_ == 4) finishAutonomousEpisode(now);
      return;
    case AutonomousEpisode::SOCIAL_ATTENTION:
      finishAutonomousEpisode(now);
      return;
    case AutonomousEpisode::ALERT_CHECK:
      if (autonomousEpisodeStep_ == 0) {
        autonomousEpisodeStep_ = 1;
        beginAutonomousEpisodeGaze(now, static_cast<int16_t>(-autonomousEpisodeDirection_ * 12),
                                   static_cast<int16_t>(random(-3, 4)), 420);
      } else if (autonomousEpisodeStep_ == 1) {
        autonomousEpisodeStep_ = 2;
        beginAutonomousEpisodeGaze(now, 0, 0, 650);
      } else finishAutonomousEpisode(now);
      return;
    case AutonomousEpisode::WAITING: return;
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
      if (autonomousEpisode_ == AutonomousEpisode::DROWSY_REST) {
        setAutonomousExpression(AutonomousExpression::SLEEPING, 100);
        beginAutonomousEpisodeGaze(now, 0, 4, static_cast<uint16_t>(random(4'200, 6'501)));
      } else {
        beginAutonomousEpisodeGaze(now, 0, 0, 1'100);
      }
    }
    return;
  }
  if (autonomousGazePhase_ == AutonomousGazePhase::FIXATING &&
      static_cast<int32_t>(now - fixationUntilMs_) >= 0) advanceAutonomousEpisode(now);
}

uint32_t NewoDisplay::adjustAutonomousFixation(uint32_t fixationMs) const {
  int32_t adjusted = static_cast<int32_t>(fixationMs);
  const uint8_t energy = autonomyState_.energy();
  const uint8_t curiosity = autonomyState_.curiosity();
  const uint8_t social = autonomyState_.social();
  const uint8_t stress = autonomyState_.stress();
  if (energy < 70) adjusted += static_cast<int32_t>(70 - energy) * 10;
  if (energy > 75) adjusted -= static_cast<int32_t>(energy - 75) * 7;
  if (curiosity > NewoAutonomyState::kCuriosityBaseline) {
    adjusted -= static_cast<int32_t>(curiosity - NewoAutonomyState::kCuriosityBaseline) * 2;
  }
  if (social > 60) adjusted += static_cast<int32_t>(social - 60) * 3;
  if (stress > 0) adjusted -= static_cast<int32_t>(stress) * 3;
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
  const uint8_t energy = autonomyState_.energy();
  const uint8_t stress = autonomyState_.stress();
  int32_t intervalMs = random(2'500, 6'001);
  if (energy < 55) intervalMs += static_cast<int32_t>(55 - energy) * 12;
  if (energy > 80) intervalMs -= static_cast<int32_t>(energy - 80) * 8;
  if (stress > 8) intervalMs -= static_cast<int32_t>(stress - 8) * 10;
  if (intervalMs < 2'200) intervalMs = 2'200;
  if (intervalMs > 6'200) intervalMs = 6'200;
  nextBlinkMs_ = now + static_cast<uint32_t>(intervalMs);
}

void NewoDisplay::startBilateralBlink(bool allowAutonomousVariation, uint8_t forcedBlink) {
  uint8_t longBlinkChance = 2;
  if (autonomyState_.energy() < 55) ++longBlinkChance;
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
  if (!autonomousGazeLargeShift_ || blinkPhase_ != BlinkPhase::OPEN || winkActive_ ||
      blinkSchedulerState_ != BlinkSchedulerState::WAITING || random(100) >= 25) return;
  postSaccadeBlinkPending_ = true;
  nextBlinkMs_ = now + static_cast<uint32_t>(random(150, 351));
}

void NewoDisplay::chooseAutonomousGazeTarget() {
  const auto clampWeight = [](int16_t value, int16_t minimum, int16_t maximum) -> int16_t {
    if (value < minimum) return minimum;
    return value > maximum ? maximum : value;
  };
  const int16_t curiosityOffset = static_cast<int16_t>(autonomyState_.curiosity()) -
      NewoAutonomyState::kCuriosityBaseline;
  const int16_t socialOffset = static_cast<int16_t>(autonomyState_.social()) -
      NewoAutonomyState::kSocialBaseline;
  const int16_t centerWeight = clampWeight(34 + socialOffset / 4 - curiosityOffset / 6, 28, 38);
  const int16_t sideWeight = clampWeight(42 + curiosityOffset / 4 - socialOffset / 8, 38, 48);
  const int16_t upperWeight = clampWeight(18 + curiosityOffset / 6, 15, 22);
  const uint8_t energy = autonomyState_.energy();
  const int16_t sideRangeX = energy < 55 ? 16 : energy > 80 ? 20 : 18;
  const int16_t upperRangeX = energy < 55 ? 12 : energy > 80 ? 16 : 14;
  const long choice = random(100);
  if (choice < centerWeight) {
    gazeTargetX_ = static_cast<int16_t>(random(-6, 7));
    gazeTargetY_ = static_cast<int16_t>(random(-4, 5));
  } else if (choice < centerWeight + sideWeight) {
    const int16_t side = random(0, 2) ? 1 : -1;
    gazeTargetX_ = static_cast<int16_t>(side * random(12, sideRangeX + 1));
    gazeTargetY_ = static_cast<int16_t>(random(-4, 5));
  } else if (choice < centerWeight + sideWeight + upperWeight) {
    gazeTargetX_ = static_cast<int16_t>(random(-upperRangeX, upperRangeX + 1));
    gazeTargetY_ = static_cast<int16_t>(random(-12, -3));
  } else {
    gazeTargetX_ = static_cast<int16_t>(random(-8, 9));
    gazeTargetY_ = static_cast<int16_t>(random(5, 12));
  }
  const int16_t deltaX = gazeTargetX_ - gazeX_;
  const int16_t deltaY = gazeTargetY_ - gazeY_;
  const int16_t magnitudeX = deltaX < 0 ? -deltaX : deltaX;
  const int16_t magnitudeY = deltaY < 0 ? -deltaY : deltaY;
  autonomousGazeLargeShift_ = magnitudeX >= 12 || magnitudeY >= 7;
  gazeMotion_.start(gazeX_, gazeY_, gazeTargetX_, gazeTargetY_,
                    kAutonomousGazeHardX, kAutonomousGazeHardY, true);
  autonomousGazePhase_ = AutonomousGazePhase::MOVING;
}

void NewoDisplay::updateAutonomousIdleGaze(uint32_t now) {
  if (autonomousEpisode_ != AutonomousEpisode::WAITING) {
    if (autonomousGazePhase_ == AutonomousGazePhase::MOVING) {
      if (!gazeMotion_.update(gazeX_, gazeY_)) return;
      fixationUntilMs_ = now + autonomousEpisodeHoldMs_;
      autonomousGazePhase_ = AutonomousGazePhase::FIXATING;
    }
    return;
  }
  switch (autonomousGazePhase_) {
    case AutonomousGazePhase::CHOOSE_TARGET:
      chooseAutonomousGazeTarget();
      return;
    case AutonomousGazePhase::MOVING:
      if (!gazeMotion_.update(gazeX_, gazeY_)) return;
      queuePostSaccadeBlink(now);
      autonomousGazeLargeShift_ = false;
      {
        const NewoInactivityStage stage = autonomyState_.stage(now);
        const uint32_t baseFixationMs = stage == NewoInactivityStage::DROWSY ? static_cast<uint32_t>(random(2'200, 4'201))
            : stage == NewoInactivityStage::RELAXED ? static_cast<uint32_t>(random(1'500, 3'101))
                                                    : static_cast<uint32_t>(random(1'000, 2'401));
        const uint32_t fixationMs = adjustAutonomousFixation(baseFixationMs);
        fixationUntilMs_ = now + fixationMs;
        microCorrectionPending_ = random(100) < (stage == NewoInactivityStage::ACTIVE ? 28 : 18);
        const long microCorrectionEnd = static_cast<long>(fixationMs * 2 / 3);
        microCorrectionAtMs_ = microCorrectionPending_ && microCorrectionEnd > 300
            ? now + static_cast<uint32_t>(random(300, microCorrectionEnd)) : 0;
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
        gazeMotion_.start(gazeX_, gazeY_, gazeTargetX_, gazeTargetY_,
                          kAutonomousGazeHardX, kAutonomousGazeHardY, false);
        autonomousGazePhase_ = AutonomousGazePhase::MICRO_CORRECTION;
      }
      return;
    case AutonomousGazePhase::MICRO_CORRECTION:
      if (!gazeMotion_.update(gazeX_, gazeY_)) return;
      autonomousGazePhase_ = static_cast<int32_t>(now - fixationUntilMs_) >= 0
          ? AutonomousGazePhase::CHOOSE_TARGET : AutonomousGazePhase::FIXATING;
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
        if (faceStyle_ == NewoFaceStyle::CLOSED || faceStyle_ == NewoFaceStyle::SLEEPING) {
          gazeTargetX_ = 0;
          gazeTargetY_ = 0;
          nextGazeMs_ = now + 1'000;
          break;
        }
        if (faceStyle_ >= NewoFaceStyle::LOOK_LEFT && faceStyle_ <= NewoFaceStyle::LOOK_DOWN_RIGHT) {
          gazeTargetX_ = (faceStyle_ == NewoFaceStyle::LOOK_LEFT || faceStyle_ == NewoFaceStyle::LOOK_UP_LEFT ||
                          faceStyle_ == NewoFaceStyle::LOOK_DOWN_LEFT) ? -14 :
                         (faceStyle_ == NewoFaceStyle::LOOK_RIGHT || faceStyle_ == NewoFaceStyle::LOOK_UP_RIGHT ||
                          faceStyle_ == NewoFaceStyle::LOOK_DOWN_RIGHT) ? 14 : 0;
          gazeTargetY_ = (faceStyle_ == NewoFaceStyle::LOOK_UP || faceStyle_ == NewoFaceStyle::LOOK_UP_LEFT ||
                          faceStyle_ == NewoFaceStyle::LOOK_UP_RIGHT) ? -8 :
                         (faceStyle_ == NewoFaceStyle::LOOK_DOWN || faceStyle_ == NewoFaceStyle::LOOK_DOWN_LEFT ||
                          faceStyle_ == NewoFaceStyle::LOOK_DOWN_RIGHT) ? 8 : 0;
          nextGazeMs_ = now + 1'000;
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
          rangeX = 17; rangeY = 8; minDelay = 900; maxDelay = 2'201;
        } else if (faceStyle_ == NewoFaceStyle::CONFUSED) {
          rangeX = 0; rangeY = 3; minDelay = 1'400; maxDelay = 2'801;
        } else if (faceStyle_ == NewoFaceStyle::HAPPY || faceStyle_ == NewoFaceStyle::LAUGH) {
          rangeX = 10; rangeY = 4; minDelay = 1'000; maxDelay = 2'601;
        } else if (faceStyle_ == NewoFaceStyle::SKEPTICAL) {
          rangeX = 8; rangeY = 4; minDelay = 1'600; maxDelay = 3'201;
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
        gazeTargetX_ = random(0, 2) ? static_cast<int16_t>(random(8, 17)) : static_cast<int16_t>(random(-16, -7));
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

void NewoDisplay::drawFaceFrame(uint32_t now) {
  const uint32_t frameStartedUs = micros();
  eyeCanvas_.fillScreen(0);
  const NewoDisplayMode activeMode = effectiveMode(now);
  updateAutonomousEpisode(now);
  updateBlinkBeforeFrame(now, activeMode);
  updateGaze(now);

  const NewoEyePose targetPose = resolveEyePose(now, activeMode);
  eyePoseEngine_.transitionTo(targetPose, now, eyePoseTransitionMs(activeMode), eyePoseEasing(activeMode));
  const NewoEyePose& pose = eyePoseEngine_.update(now);
  const EyeMotionOverlay motion = resolveEyeMotionOverlay(now, activeMode);

  const bool cyclops = pose.rightWidth == 0;
  int16_t leftW = static_cast<int16_t>(pose.leftWidth + motion.leftWidthDelta);
  int16_t rightW = cyclops ? 0 : static_cast<int16_t>(pose.rightWidth + motion.rightWidthDelta);
  if (leftW < 2) leftW = 2;
  if (!cyclops && rightW < 2) rightW = 2;
  const int16_t gap = pose.gap;

  uint8_t blinkOpen = 100;
  if (blinkPhase_ == BlinkPhase::HALF_CLOSED || blinkPhase_ == BlinkPhase::HALF_OPEN) blinkOpen = 40;
  else if (blinkPhase_ == BlinkPhase::CLOSED) blinkOpen = 7;
  const uint16_t combinedOpen = static_cast<uint16_t>(pose.openness) * blinkOpen / 100U;
  int16_t leftBaseHeight = static_cast<int16_t>(pose.leftHeight + motion.leftHeightDelta);
  int16_t rightBaseHeight = static_cast<int16_t>(pose.rightHeight + motion.rightHeightDelta);
  if (leftBaseHeight < 2) leftBaseHeight = 2;
  if (rightBaseHeight < 2) rightBaseHeight = 2;
  int16_t leftHeight = static_cast<int16_t>(leftBaseHeight * combinedOpen / 100U);
  int16_t rightHeight = static_cast<int16_t>(rightBaseHeight * combinedOpen / 100U);
  if (pose.closureStyle == NewoEyeClosureStyle::FILLED) {
    if (leftHeight < 2) leftHeight = 2;
    if (rightHeight < 2) rightHeight = 2;
  }

  if (cyclops) {
    const int16_t x = (kEyeCanvasWidth - leftW) / 2 + gazeX_ + motion.xOffset;
    if (pose.closureStyle == NewoEyeClosureStyle::CURVED) {
      const int16_t y = kEyeCanvasHeight / 2 + gazeY_ + motion.yOffset + pose.leftYOffset;
      drawClosedEyeCurve(x, y, leftW);
    } else {
      int16_t y = (kEyeCanvasHeight - leftBaseHeight) / 2 + gazeY_ + motion.yOffset + pose.leftYOffset;
      y += (leftBaseHeight - leftHeight) / 2;
      eyeCanvas_.fillRoundRect(x, y, leftW, leftHeight, leftHeight > 3 ? leftHeight / 2 : 1, 1);
    }
  } else {
    const int16_t totalWidth = leftW + gap + rightW;
    const int16_t leftX = (kEyeCanvasWidth - totalWidth) / 2 + gazeX_ + motion.xOffset;
    const int16_t rightX = leftX + leftW + gap;
    int16_t leftY = (kEyeCanvasHeight - leftBaseHeight) / 2 + gazeY_ + motion.yOffset + pose.leftYOffset;
    int16_t rightY = (kEyeCanvasHeight - rightBaseHeight) / 2 + gazeY_ + motion.yOffset + pose.rightYOffset;
    leftY += (leftBaseHeight - leftHeight) / 2;
    rightY += (rightBaseHeight - rightHeight) / 2;

    if (winkActive_ && pose.closureStyle == NewoEyeClosureStyle::FILLED) {
      const uint32_t winkElapsedMs = now - winkStartedMs_;
      const uint8_t winkOpen = (winkElapsedMs < 60 || winkElapsedMs >= 180) ? 40 : 7;
      if (winkLeft_) {
        leftHeight = static_cast<int16_t>(leftBaseHeight * pose.openness * winkOpen / 10'000U);
        if (leftHeight < 2) leftHeight = 2;
        leftY = (kEyeCanvasHeight - leftBaseHeight) / 2 + gazeY_ + motion.yOffset + pose.leftYOffset +
                (leftBaseHeight - leftHeight) / 2;
      } else {
        rightHeight = static_cast<int16_t>(rightBaseHeight * pose.openness * winkOpen / 10'000U);
        if (rightHeight < 2) rightHeight = 2;
        rightY = (kEyeCanvasHeight - rightBaseHeight) / 2 + gazeY_ + motion.yOffset + pose.rightYOffset +
                 (rightBaseHeight - rightHeight) / 2;
      }
    }

    if (pose.closureStyle == NewoEyeClosureStyle::CURVED) {
      drawClosedEyeCurve(leftX, static_cast<int16_t>(kEyeCanvasHeight / 2 + gazeY_ + motion.yOffset + pose.leftYOffset), leftW);
      drawClosedEyeCurve(rightX, static_cast<int16_t>(kEyeCanvasHeight / 2 + gazeY_ + motion.yOffset + pose.rightYOffset), rightW);
    } else {
      eyeCanvas_.fillRoundRect(leftX, leftY, leftW, leftHeight, leftHeight > 3 ? leftHeight / 2 : 1, 1);
      eyeCanvas_.fillRoundRect(rightX, rightY, rightW, rightHeight, rightHeight > 3 ? rightHeight / 2 : 1, 1);
      applyResolvedPoseCuts(leftX, rightX, leftY, rightY, leftW, rightW, leftHeight, rightHeight, pose);
    }
  }

  drawSecondaryEffect(now, secondaryEffectFor(now, activeMode));
  blitMonoCanvasFast(eyeCanvas_, kEyeCanvasX, kEyeCanvasY, kEyeCanvasWidth, kEyeCanvasHeight);
  advanceBlinkAfterFrame(now);
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
    response[44] = '.'; response[45] = '.'; response[46] = '.'; response[47] = '\0';
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
    if (*cursor == '\n') { words = 3; break; }
    if (*cursor != ' ' && !inWord) { ++words; inWord = true; }
    if (*cursor == ' ') inWord = false;
  }
  const bool shortMessage = words > 0 && words <= 2 && strlen(text_) <= 14;
  if (shortMessage) {
    display_.setFont(&FreeSans18pt7b);
    int16_t x1, y1; uint16_t w, h;
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
  int16_t x1, y1; uint16_t w, h;
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
    if (centered) drawCentered(line, y);
    else { display_.setCursor(kMargin, y); display_.print(line); }
    display_.setFont(info ? &FreeMono9pt7b : &FreeSans9pt7b);
    y += lineHeight;
    ++lines;
    length = 0;
  };
  while (*cursor && y <= 230 && (!maxLines || lines < maxLines)) {
    if (*cursor == '\n') { flush(); ++cursor; continue; }
    while (*cursor == ' ') ++cursor;
    if (!*cursor) break;
    if (*cursor == '\n') { flush(); ++cursor; continue; }
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
    int16_t x1, y1; uint16_t width, height;
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
