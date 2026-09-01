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
constexpr uint32_t kAutonomousBehaviorMinMs = 4'000;
constexpr uint32_t kAutonomousBehaviorMaxMs = 9'000;
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
  resetFaceMotion(now);
  render();
}

bool NewoDisplay::setMode(NewoDisplayMode mode, const char* text, bool temporary) {
  if (mode > NewoDisplayMode::ECO) return false;
  if (mode_ != mode) {
    mode_ = mode;
    modeStartedMs_ = millis();
    resetFaceMotion(modeStartedMs_);
    switch (mode_) {
      case NewoDisplayMode::LISTENING:
        noteInteraction(modeStartedMs_, 3, 5, 4);
        break;
      case NewoDisplayMode::THINKING:
        noteInteraction(modeStartedMs_, 2, 3, 3);
        break;
      case NewoDisplayMode::SPEAKING:
        noteInteraction(modeStartedMs_, 2, 2, 3);
        break;
      case NewoDisplayMode::ERROR:
        noteError();
        break;
      default:
        break;
    }
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
  restoreAtMs_ = temporary ? millis() + kTemporaryMs : 0;
  dirty_ = true;
  return true;
}

void NewoDisplay::setSpeakerActive(bool active) {
  if (active == speakerActive_) return;
  // Do not dirty/redraw text or ECO pages. The normal face frame path observes
  // this hint and can adjust only its bounded animation cadence/activity mark.
  speakerActive_ = active;
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
  mode_ = NewoDisplayMode::IDLE;
  persistentMode_ = NewoDisplayMode::IDLE;
  text_[0] = '\0';
  persistentText_[0] = '\0';
  temporary_ = false;
  restoreAtMs_ = 0;
  ecoEnabled_ = false;
  ecoPage_ = 0;
  nextEcoPageMs_ = 0;
  modeStartedMs_ = millis();
  if (styleChanged) noteInteraction(modeStartedMs_, 1, 2, 2);
  resetFaceMotion(modeStartedMs_);
  dirty_ = true;
  return true;
}

void NewoDisplay::toggleEco() {
  const uint32_t now = millis();
  temporary_ = false;
  if (!ecoEnabled_) {
    ecoEnabled_ = true;
    mode_ = NewoDisplayMode::ECO;
    resetAutonomousBehavior(now);
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
  if (ecoEnabled_ && !temporary_ && static_cast<int32_t>(now - nextEcoPageMs_) >= 0) {
    nextEcoPageMs_ = now + kEcoPageMs;
    ecoPage_ = (ecoPage_ + 1) % 3;
    dirty_ = true;
  }
  updateAutonomousState(now);
  if (dirty_) render();
  if (!temporary_ && mode_ != NewoDisplayMode::ECO && mode_ != NewoDisplayMode::MESSAGE &&
      static_cast<int32_t>(now - nextFaceFrameMs_) >= 0) {
    // Audio playback is only a hint: lower face-frame SPI work without taking
    // ownership of the active display mode, text, ECO page, or timer.
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
  resetAutonomousBehavior(now);
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
  stress_ = saturatingAdd(stress_, 12);
}

void NewoDisplay::updateAutonomousState(uint32_t now) {
  if (static_cast<int32_t>(now - nextAutonomousStateMs_) >= 0) {
    nextAutonomousStateMs_ = now + kAutonomousStateUpdateMs;
    if (curiosity_ > kCuriosityBaseline) --curiosity_;
    if (curiosity_ < kCuriosityBaseline) ++curiosity_;
    if (stress_ > 0) --stress_;

    if (isOngoingEngagement(mode_)) {
      // Active conversation refreshes only inactivity; entry transitions own personality gains.
      lastInteractionMs_ = now;
      idleDriftTicks_ = 0;
    } else if (now - lastInteractionMs_ >= kInactivityBeforeDriftMs) {
      if (++idleDriftTicks_ >= 4) {
        idleDriftTicks_ = 0;
        if (energy_ > 40) --energy_;
        if (social_ > 25) --social_;
      }
    } else {
      idleDriftTicks_ = 0;
    }
  }

  if (static_cast<int32_t>(now - nextAutonomousStateLogMs_) >= 0) {
    Serial.printf("[system] EYES_STATE energy=%u curiosity=%u social=%u stress=%u\n",
                  static_cast<unsigned>(energy_), static_cast<unsigned>(curiosity_),
                  static_cast<unsigned>(social_), static_cast<unsigned>(stress_));
    nextAutonomousStateLogMs_ = now + kAutonomousStateLogMs;
  }
}

void NewoDisplay::resetAutonomousBehavior(uint32_t now) {
  autonomousBehavior_ = AutonomousBehavior::WAITING;
  autonomousBehaviorUntilMs_ = 0;
  autonomousBehaviorHoldMs_ = 0;
  autonomousBehaviorReturningToCenter_ = false;
  autonomousBehaviorBlinkStarted_ = false;
  if (autonomousIdle()) scheduleNextAutonomousBehavior(now);
  else nextAutonomousBehaviorMs_ = 0;
}

void NewoDisplay::scheduleNextAutonomousBehavior(uint32_t now) {
  if (!autonomousIdle()) {
    nextAutonomousBehaviorMs_ = 0;
    return;
  }

  int32_t minimumMs = kAutonomousBehaviorMinMs;
  int32_t maximumMs = kAutonomousBehaviorMaxMs;
  const uint32_t inactiveMs = now - lastInteractionMs_;
  if (energy_ > 80) maximumMs -= 500;
  else if (energy_ < 55) minimumMs += 300;
  if (inactiveMs >= kDrowsyInactivityMs) minimumMs += 1'000;
  else if (inactiveMs >= kRelaxedInactivityMs) minimumMs += 500;
  if (minimumMs > 8'500) minimumMs = 8'500;
  if (maximumMs < minimumMs + 1) maximumMs = minimumMs + 1;
  nextAutonomousBehaviorMs_ = now + static_cast<uint32_t>(random(minimumMs, maximumMs + 1));
}

void NewoDisplay::finishAutonomousBehavior(uint32_t now) {
  autonomousBehavior_ = AutonomousBehavior::WAITING;
  autonomousBehaviorUntilMs_ = 0;
  autonomousBehaviorHoldMs_ = 0;
  autonomousBehaviorReturningToCenter_ = false;
  autonomousBehaviorBlinkStarted_ = false;
  scheduleNextAutonomousBehavior(now);
}

void NewoDisplay::beginAutonomousBehaviorGaze(uint32_t now, int16_t targetX, int16_t targetY,
                                               uint16_t holdMs) {
  gazeTargetX_ = targetX;
  gazeTargetY_ = targetY;
  autonomousGazeLargeShift_ = false;
  microCorrectionPending_ = false;
  microCorrectionAtMs_ = 0;
  autonomousBehaviorHoldMs_ = holdMs;
  autonomousBehaviorUntilMs_ = 0;
  autonomousBehaviorReturningToCenter_ = false;
  autonomousGazePhase_ = AutonomousGazePhase::MOVING;
  nextGazeMs_ = now;
}

void NewoDisplay::chooseAutonomousBehavior(uint32_t now) {
  if (!autonomousIdle() || blinkPhase_ != BlinkPhase::OPEN || winkActive_ ||
      blinkSchedulerState_ != BlinkSchedulerState::WAITING || postSaccadeBlinkPending_ ||
      static_cast<int32_t>(now - nextBlinkMs_) >= 0) {
    scheduleNextAutonomousBehavior(now);
    return;
  }

  const uint32_t inactiveMs = now - lastInteractionMs_;
  const InactivityStage stage = inactiveMs >= kDrowsyInactivityMs ? InactivityStage::DROWSY
                              : inactiveMs >= kRelaxedInactivityMs ? InactivityStage::RELAXED
                                                                    : InactivityStage::ACTIVE;
  int noneWeight = 70;
  int gazeWeight = 16;
  int blinkWeight = 7;
  int expressionWeight = 5;
  int winkWeight = 1;
  int restWeight = 1;
  const auto shiftWeight = [](int& from, int& to, int amount) {
    if (amount > from) amount = from;
    from -= amount;
    to += amount;
  };

  if (energy_ > 80) shiftWeight(noneWeight, gazeWeight, 3);
  else if (energy_ < 55) {
    shiftWeight(gazeWeight, noneWeight, 2);
    shiftWeight(noneWeight, blinkWeight, 1);
    shiftWeight(noneWeight, restWeight, 1);
  }
  if (curiosity_ > 55) {
    shiftWeight(noneWeight, gazeWeight, 3);
    shiftWeight(noneWeight, expressionWeight, 1);
  }
  if (social_ > 55) shiftWeight(gazeWeight, noneWeight, 2);
  if (stress_ > 8) {
    shiftWeight(noneWeight, gazeWeight, 2);
    shiftWeight(noneWeight, blinkWeight, 2);
  }
  if (stage == InactivityStage::RELAXED) {
    shiftWeight(gazeWeight, noneWeight, 3);
    shiftWeight(expressionWeight, noneWeight, 1);
    shiftWeight(noneWeight, blinkWeight, 1);
  } else if (stage == InactivityStage::DROWSY) {
    shiftWeight(gazeWeight, noneWeight, 4);
    shiftWeight(expressionWeight, restWeight, 2);
    shiftWeight(noneWeight, blinkWeight, 1);
  }

  const long choice = random(100);
  int threshold = noneWeight;
  if (choice < threshold) {
    scheduleNextAutonomousBehavior(now);
    return;
  }
  threshold += gazeWeight;
  if (choice < threshold) {
    int16_t rangeX = stage == InactivityStage::DROWSY ? 5 : stage == InactivityStage::RELAXED ? 9 : 13;
    if (energy_ < 55 && rangeX > 5) --rangeX;
    if (energy_ > 80 && rangeX < 14) ++rangeX;
    if (social_ > 55 && rangeX > 5) --rangeX;
    const int16_t centerChance = social_ > 55 ? 28 : 8;
    const int16_t downChance = stage == InactivityStage::DROWSY ? 15 : 4;
    const int16_t upChance = stage == InactivityStage::DROWSY ? 8 : curiosity_ > 55 ? 24 : 14;
    const long direction = random(100);
    const uint16_t holdMs = stage == InactivityStage::DROWSY ? static_cast<uint16_t>(random(1'200, 2'201))
                          : stage == InactivityStage::RELAXED ? static_cast<uint16_t>(random(900, 1'701))
                                                               : static_cast<uint16_t>(random(500, 1'101));
    if (direction < centerChance) {
      autonomousBehavior_ = AutonomousBehavior::CENTER_FIXATION;
      beginAutonomousBehaviorGaze(now, 0, 0, static_cast<uint16_t>(holdMs + 500));
    } else if (direction < centerChance + downChance) {
      autonomousBehavior_ = AutonomousBehavior::GLANCE_DOWN;
      beginAutonomousBehaviorGaze(now, static_cast<int16_t>(random(-rangeX / 2, rangeX / 2 + 1)),
                                  static_cast<int16_t>(random(3, 7)), holdMs);
    } else if (direction < centerChance + downChance + upChance) {
      autonomousBehavior_ = AutonomousBehavior::GLANCE_UP;
      beginAutonomousBehaviorGaze(now, static_cast<int16_t>(random(-rangeX / 2, rangeX / 2 + 1)),
                                  static_cast<int16_t>(random(-7, -3)), holdMs);
    } else if (random(0, 2) == 0) {
      autonomousBehavior_ = AutonomousBehavior::GLANCE_LEFT;
      beginAutonomousBehaviorGaze(now, -rangeX, static_cast<int16_t>(random(-2, 3)), holdMs);
    } else {
      autonomousBehavior_ = AutonomousBehavior::GLANCE_RIGHT;
      beginAutonomousBehaviorGaze(now, rangeX, static_cast<int16_t>(random(-2, 3)), holdMs);
    }
    return;
  }
  threshold += blinkWeight;
  if (choice < threshold) {
    autonomousBehavior_ = (stage == InactivityStage::DROWSY || energy_ < 55 || random(100) < 45)
        ? AutonomousBehavior::LONG_BLINK
        : AutonomousBehavior::DOUBLE_BLINK;
    return;
  }
  threshold += expressionWeight;
  if (choice < threshold) {
    autonomousBehavior_ = curiosity_ > 55 && random(100) < 65
        ? AutonomousBehavior::CURIOSITY_LIFT
        : AutonomousBehavior::HAPPY_SQUINT;
    autonomousBehaviorUntilMs_ = now + static_cast<uint32_t>(random(350, 601));
    return;
  }
  threshold += winkWeight;
  if (choice < threshold) {
    autonomousBehavior_ = AutonomousBehavior::WINK;
    winkActive_ = true;
    winkLeft_ = random(0, 2) == 0;
    winkStartedMs_ = now;
    return;
  }
  (void)restWeight;
  autonomousBehavior_ = AutonomousBehavior::REST_CLOSE;
  autonomousBehaviorUntilMs_ = now + static_cast<uint32_t>(random(500, 1'201));
}

void NewoDisplay::updateAutonomousBehavior(uint32_t now) {
  if (!autonomousIdle()) {
    if (autonomousBehavior_ != AutonomousBehavior::WAITING) resetAutonomousBehavior(now);
    return;
  }

  switch (autonomousBehavior_) {
    case AutonomousBehavior::WAITING:
      if (static_cast<int32_t>(now - nextAutonomousBehaviorMs_) >= 0) chooseAutonomousBehavior(now);
      return;
    case AutonomousBehavior::GLANCE_LEFT:
    case AutonomousBehavior::GLANCE_RIGHT:
    case AutonomousBehavior::GLANCE_UP:
    case AutonomousBehavior::GLANCE_DOWN:
    case AutonomousBehavior::CENTER_FIXATION:
      if (autonomousBehaviorUntilMs_ == 0 || static_cast<int32_t>(now - autonomousBehaviorUntilMs_) < 0) return;
      if (autonomousBehavior_ == AutonomousBehavior::CENTER_FIXATION) {
        autonomousGazePhase_ = AutonomousGazePhase::CHOOSE_TARGET;
        finishAutonomousBehavior(now);
      } else {
        gazeTargetX_ = 0;
        gazeTargetY_ = 0;
        autonomousGazeLargeShift_ = false;
        microCorrectionPending_ = false;
        autonomousBehaviorReturningToCenter_ = true;
        autonomousGazePhase_ = AutonomousGazePhase::MOVING;
      }
      return;
    case AutonomousBehavior::DOUBLE_BLINK:
    case AutonomousBehavior::LONG_BLINK:
      if (autonomousBehaviorBlinkStarted_ && blinkPhase_ == BlinkPhase::OPEN &&
          blinkSchedulerState_ == BlinkSchedulerState::WAITING) finishAutonomousBehavior(now);
      return;
    case AutonomousBehavior::CURIOSITY_LIFT:
    case AutonomousBehavior::HAPPY_SQUINT:
    case AutonomousBehavior::REST_CLOSE:
      if (static_cast<int32_t>(now - autonomousBehaviorUntilMs_) >= 0) {
        const bool restCloseCanResetBlink = autonomousBehavior_ == AutonomousBehavior::REST_CLOSE &&
                                            blinkPhase_ == BlinkPhase::OPEN &&
                                            blinkSchedulerState_ == BlinkSchedulerState::WAITING &&
                                            !postSaccadeBlinkPending_ && !winkActive_;
        finishAutonomousBehavior(now);
        if (restCloseCanResetBlink) scheduleNextBilateralBlink(now);
      }
      return;
    case AutonomousBehavior::WINK:
      if (!winkActive_) finishAutonomousBehavior(now);
      return;
  }
}

uint32_t NewoDisplay::adjustAutonomousFixation(uint32_t fixationMs) const {
  int32_t adjusted = static_cast<int32_t>(fixationMs);
  if (energy_ < 55) adjusted += static_cast<int32_t>(55 - energy_) * 8;
  if (energy_ > 80) adjusted -= static_cast<int32_t>(energy_ - 80) * 6;
  if (curiosity_ > 55) adjusted -= static_cast<int32_t>(curiosity_ - 55) * 4;
  if (stress_ > 8) adjusted -= static_cast<int32_t>(stress_ - 8) * 6;
  if (adjusted < 300) return 300;
  if (adjusted > 3'500) return 3'500;
  return static_cast<uint32_t>(adjusted);
}

bool NewoDisplay::autonomousIdle() const {
  return mode_ == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::NEUTRAL;
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
  blinkSchedulerState_ = forcedBlink == kForceDoubleBlink
      ? BlinkSchedulerState::DOUBLE_PAUSE
      : allowAutonomousVariation && !longBlink_ && random(100) < 7
          ? BlinkSchedulerState::DOUBLE_PAUSE
          : BlinkSchedulerState::WAITING;
  postSaccadeBlinkPending_ = false;
  blinkPhase_ = BlinkPhase::HALF_CLOSED;
  blinkFramesRemaining_ = 1;
}

void NewoDisplay::queuePostSaccadeBlink(uint32_t now) {
  if (autonomousBehavior_ != AutonomousBehavior::WAITING || !autonomousGazeLargeShift_ ||
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
  const int16_t centerWeight = clampWeight(52 + socialOffset / 3 - curiosityOffset / 4, 45, 60);
  int16_t sideWeight = clampWeight(30 + curiosityOffset / 4 - socialOffset / 5, 24, 35);
  // Reserve at least 10% upper attention and 4% downward attention.
  if (centerWeight + sideWeight > 86) sideWeight = 86 - centerWeight;
  int16_t upperWeight = clampWeight(13 + curiosityOffset / 8, 10, 16);
  const int16_t maximumUpperWeight = 96 - centerWeight - sideWeight;
  if (upperWeight > maximumUpperWeight) upperWeight = maximumUpperWeight;
  const int16_t energyRange = energy_ < 55 ? -1 : energy_ > 80 ? 1 : 0;
  const int16_t socialRangeLimit = social_ > 55 ? 1 : 0;
  const int16_t centerRangeX = clampWeight(5 + energyRange, 4, 6);
  const int16_t centerRangeY = clampWeight(3 + energyRange, 2, 4);
  const int16_t sideRangeX = clampWeight(14 + energyRange - socialRangeLimit, 10, 14);
  const int16_t upperRangeX = clampWeight(8 + energyRange - socialRangeLimit, 6, 9);

  const long choice = random(100);
  if (choice < centerWeight) {
    // Social attention reinforces forward fixation; curiosity shifts weight outward.
    gazeTargetX_ = static_cast<int16_t>(random(-centerRangeX, centerRangeX + 1));
    gazeTargetY_ = static_cast<int16_t>(random(-centerRangeY, centerRangeY + 1));
  } else if (choice < centerWeight + sideWeight) {
    const int16_t side = random(0, 2) ? 1 : -1;
    gazeTargetX_ = static_cast<int16_t>(side * random(8, sideRangeX + 1));
    gazeTargetY_ = static_cast<int16_t>(random(-centerRangeY, centerRangeY + 1));
  } else if (choice < centerWeight + sideWeight + upperWeight) {
    gazeTargetX_ = static_cast<int16_t>(random(-upperRangeX, upperRangeX + 1));
    gazeTargetY_ = static_cast<int16_t>(random(-8, -3));
  } else {
    // Downward glances stay uncommon regardless of personality state.
    gazeTargetX_ = static_cast<int16_t>(random(-centerRangeX - 1, centerRangeX + 2));
    gazeTargetY_ = static_cast<int16_t>(random(4, 8));
  }
  const int16_t deltaX = gazeTargetX_ - gazeX_;
  const int16_t deltaY = gazeTargetY_ - gazeY_;
  const int16_t magnitudeX = deltaX < 0 ? -deltaX : deltaX;
  const int16_t magnitudeY = deltaY < 0 ? -deltaY : deltaY;
  autonomousGazeLargeShift_ = magnitudeX >= 11 || magnitudeY >= 6;
  autonomousGazePhase_ = AutonomousGazePhase::MOVING;
}

void NewoDisplay::updateAutonomousIdleGaze(uint32_t now) {
  const bool behaviorOwnsGaze = autonomousBehavior_ == AutonomousBehavior::GLANCE_LEFT ||
                                autonomousBehavior_ == AutonomousBehavior::GLANCE_RIGHT ||
                                autonomousBehavior_ == AutonomousBehavior::GLANCE_UP ||
                                autonomousBehavior_ == AutonomousBehavior::GLANCE_DOWN ||
                                autonomousBehavior_ == AutonomousBehavior::CENTER_FIXATION;
  switch (autonomousGazePhase_) {
    case AutonomousGazePhase::CHOOSE_TARGET:
      chooseAutonomousGazeTarget();
      return;
    case AutonomousGazePhase::MOVING:
      gazeX_ = easeAutonomousGaze(gazeX_, gazeTargetX_);
      gazeY_ = easeAutonomousGaze(gazeY_, gazeTargetY_);
      if (gazeX_ != gazeTargetX_ || gazeY_ != gazeTargetY_) return;
      if (behaviorOwnsGaze) {
        if (autonomousBehaviorReturningToCenter_) {
          autonomousGazePhase_ = AutonomousGazePhase::CHOOSE_TARGET;
          finishAutonomousBehavior(now);
          return;
        }
        fixationUntilMs_ = now + autonomousBehaviorHoldMs_;
        autonomousBehaviorUntilMs_ = fixationUntilMs_;
        microCorrectionPending_ = false;
        microCorrectionAtMs_ = 0;
        autonomousGazePhase_ = AutonomousGazePhase::FIXATING;
        return;
      }
      queuePostSaccadeBlink(now);
      autonomousGazeLargeShift_ = false;
      {
        const long durationChoice = random(100);
        const uint32_t baseFixationMs = durationChoice < 15 ? static_cast<uint32_t>(random(350, 701))
                                        : durationChoice < 25 ? static_cast<uint32_t>(random(2'200, 3'501))
                                                              : static_cast<uint32_t>(random(700, 2'201));
        const uint32_t fixationMs = adjustAutonomousFixation(baseFixationMs);
        fixationUntilMs_ = now + fixationMs;
        microCorrectionPending_ = random(100) < 30;
        microCorrectionAtMs_ = microCorrectionPending_
            ? now + static_cast<uint32_t>(random(200, static_cast<long>(fixationMs * 2 / 3)))
            : 0;
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
          gazeTargetX_ = candidate >= -14 && candidate <= 14 ? candidate : gazeTargetX_ - direction * shift;
        } else {
          const int16_t candidate = gazeTargetY_ + direction * shift;
          gazeTargetY_ = candidate >= -8 && candidate <= 8 ? candidate : gazeTargetY_ - direction * shift;
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
  if (mode_ == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::NEUTRAL) {
    updateAutonomousIdleGaze(now);
    return;
  }
  if (static_cast<int32_t>(now - nextGazeMs_) >= 0) {
    switch (mode_) {
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
                                     int16_t height) {
  if (height < 8) return;

  bool tired = mode_ == NewoDisplayMode::THINKING;
  bool happy = mode_ == NewoDisplayMode::SPEAKING;
  bool angry = mode_ == NewoDisplayMode::ERROR;
  if (mode_ == NewoDisplayMode::IDLE) {
    tired = faceStyle_ == NewoFaceStyle::TIRED || faceStyle_ == NewoFaceStyle::SLEEPY;
    happy = faceStyle_ == NewoFaceStyle::HAPPY || faceStyle_ == NewoFaceStyle::LAUGH ||
            (faceStyle_ == NewoFaceStyle::NEUTRAL && autonomousBehavior_ == AutonomousBehavior::HAPPY_SQUINT);
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

  const bool winkStyle = mode_ == NewoDisplayMode::IDLE &&
                         (faceStyle_ == NewoFaceStyle::WINK_LEFT || faceStyle_ == NewoFaceStyle::WINK_RIGHT);
  if (winkActive_ && now - winkStartedMs_ >= kWinkBurstMs) {
    winkActive_ = false;
    nextWinkMs_ = now + static_cast<uint32_t>(random(kWinkCalmMinMs, kWinkCalmMaxMs));
    // Do not start a delayed autoblink immediately after a wink burst.
    nextBlinkMs_ = now + static_cast<uint32_t>(random(2'500, 5'501));
  }
  updateAutonomousBehavior(now);
  const bool deliberatelyClosed = mode_ == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::CLOSED;
  const bool phaseDBehaviorActive = autonomousBehavior_ != AutonomousBehavior::WAITING;
  if (blinkPhase_ == BlinkPhase::OPEN && !winkActive_) {
    if ((autonomousBehavior_ == AutonomousBehavior::DOUBLE_BLINK ||
         autonomousBehavior_ == AutonomousBehavior::LONG_BLINK) && !autonomousBehaviorBlinkStarted_) {
      startBilateralBlink(false, autonomousBehavior_ == AutonomousBehavior::DOUBLE_BLINK
                                     ? kForceDoubleBlink : kForceLongBlink);
      autonomousBehaviorBlinkStarted_ = true;
    } else if (autonomousBehavior_ == AutonomousBehavior::DOUBLE_BLINK && autonomousBehaviorBlinkStarted_ &&
               blinkSchedulerState_ == BlinkSchedulerState::DOUBLE_SECOND &&
               static_cast<int32_t>(now - nextBlinkMs_) >= 0) {
      startBilateralBlink(false);
    } else if (!phaseDBehaviorActive && winkStyle && static_cast<int32_t>(now - nextWinkMs_) >= 0) {
      // A wink wins over and cancels any pending bilateral sequence.
      blinkSchedulerState_ = BlinkSchedulerState::WAITING;
      longBlink_ = false;
      postSaccadeBlinkPending_ = false;
      winkActive_ = true;
      winkLeft_ = faceStyle_ == NewoFaceStyle::WINK_LEFT;
      winkStartedMs_ = now;
    } else if (!phaseDBehaviorActive && !deliberatelyClosed && static_cast<int32_t>(now - nextBlinkMs_) >= 0) {
      // One scheduler owns bilateral events; Phase D only requests this existing scheduler.
      const bool doubleSecond = blinkSchedulerState_ == BlinkSchedulerState::DOUBLE_SECOND;
      startBilateralBlink(autonomousIdle() && !postSaccadeBlinkPending_ && !doubleSecond);
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

  if (mode_ == NewoDisplayMode::IDLE) {
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
    switch (mode_) {
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
  const bool microCuriosityLift = mode_ == NewoDisplayMode::IDLE &&
                                  faceStyle_ == NewoFaceStyle::NEUTRAL &&
                                  autonomousBehavior_ == AutonomousBehavior::CURIOSITY_LIFT;
  if (mode_ == NewoDisplayMode::IDLE && (faceStyle_ == NewoFaceStyle::CURIOUS || microCuriosityLift)) {
    // RoboEyes curiosity is directional: the outer eye grows vertically when
    // the gaze reaches an edge rather than merely widening both eyes.
    const int16_t lift = microCuriosityLift ? 4 : 8;
    if (microCuriosityLift || gazeX_ < -7) curiousLeftLift = lift;
    if (microCuriosityLift || gazeX_ > 7) curiousRightLift = lift;
  } else if (mode_ == NewoDisplayMode::THINKING) {
    if (gazeX_ < -7) leftW += 7;
    if (gazeX_ > 7) rightW += 7;
  }

  int16_t shake = 0;
  if (mode_ == NewoDisplayMode::ERROR && now - modeStartedMs_ < 520) {
    shake = static_cast<int16_t>(sinf(static_cast<float>(now - modeStartedMs_) * 0.075f) * 4.0f);
  }
  const uint32_t styleBurstMs = (now - modeStartedMs_) % 2200; // selection-relative 500 ms burst, then calm.
  if (mode_ == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::CONFUSED && styleBurstMs < 500) {
    // Upstream anim_confused: approximately 20 px horizontal flicker.
    shake += static_cast<int16_t>(sinf(static_cast<float>(styleBurstMs) * 0.075f) * 20.0f);
  }
  if (mode_ == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::LAUGH && styleBurstMs < 500) {
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
  if (autonomousBehavior_ == AutonomousBehavior::REST_CLOSE) {
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
    applyEyeExpression(leftX, rightX, y, leftW, rightW, height);

    if (mode_ == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::SWEAT && height >= 8) {
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
  if (mode_ == NewoDisplayMode::LISTENING) {
    for (int8_t i = 0; i < 7; ++i) {
      const int16_t height = 5 + static_cast<int16_t>((sinf(phase * 2.0f + i * 0.8f) + 1.0f) * 5.5f);
      activityCanvas_.fillRect(12 + i * 12, 16 - height, 5, height, 1);
    }
  } else if (mode_ == NewoDisplayMode::THINKING) {
    for (int8_t i = 0; i < 3; ++i) {
      const int16_t rise = static_cast<int16_t>((sinf(phase * 1.5f + i * 1.4f) + 1.0f) * 3.0f);
      activityCanvas_.fillCircle(36 + i * 12, 15 - rise, 2, 1);
    }
  } else if (mode_ == NewoDisplayMode::SPEAKING || (speakerActive_ && mode_ == NewoDisplayMode::IDLE)) {
    int16_t lastY = 172;
    for (int16_t x = 0; x <= 72; x += 4) {
      const int16_t y = 172 + static_cast<int16_t>(sinf(phase * 2.0f + x * 0.16f) * 6.0f);
      activityCanvas_.drawLine(x - 4, lastY - 160, x, y - 160, 1);
      lastY = y;
    }
  } else if (mode_ == NewoDisplayMode::ERROR) {
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
