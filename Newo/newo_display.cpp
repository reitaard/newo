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
  resetFaceMotion(millis());
  render();
}

bool NewoDisplay::setMode(NewoDisplayMode mode, const char* text, bool temporary) {
  if (mode > NewoDisplayMode::ECO) return false;
  if (mode_ != mode) {
    modeStartedMs_ = millis();
    resetFaceMotion(modeStartedMs_);
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

bool NewoDisplay::setFaceStyle(NewoFaceStyle style) {
  if (style > NewoFaceStyle::SLEEPY) return false;
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
  resetFaceMotion(modeStartedMs_);
  dirty_ = true;
  return true;
}

void NewoDisplay::toggleEco() {
  temporary_ = false;
  if (!ecoEnabled_) {
    ecoEnabled_ = true;
    mode_ = NewoDisplayMode::ECO;
    text_[0] = '\0';
    ecoPage_ = 0;
    nextEcoPageMs_ = millis() + kEcoPageMs;
  } else {
    ecoEnabled_ = false;
    mode_ = persistentMode_;
    resetFaceMotion(millis());
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
  blinkFramesRemaining_ = 0;
  verificationBlink_ = true;
  nextBlinkMs_ = now + 1'000;
  nextWinkMs_ = now + static_cast<uint32_t>(random(kWinkCalmMinMs, kWinkCalmMaxMs));
  winkStartedMs_ = 0;
  winkActive_ = false;
  autonomousGazePhase_ = AutonomousGazePhase::CHOOSE_TARGET;
  fixationUntilMs_ = 0;
  microCorrectionAtMs_ = 0;
  microCorrectionPending_ = false;
  gazeX_ = 0;
  gazeY_ = 0;
  gazeTargetX_ = 0;
  gazeTargetY_ = 0;
  nextGazeMs_ = now;
  nextFaceFrameMs_ = 0;
}

void NewoDisplay::chooseAutonomousGazeTarget() {
  const long choice = random(100);
  if (choice < 52) {
    // Most idle glances stay near the forward/center area.
    gazeTargetX_ = static_cast<int16_t>(random(-5, 6));
    gazeTargetY_ = static_cast<int16_t>(random(-3, 4));
  } else if (choice < 82) {
    // Side attention is common, but still less likely than center fixation.
    const int16_t side = random(0, 2) ? 1 : -1;
    gazeTargetX_ = static_cast<int16_t>(side * random(8, 15));
    gazeTargetY_ = static_cast<int16_t>(random(-3, 4));
  } else if (choice < 95) {
    gazeTargetX_ = static_cast<int16_t>(random(-8, 9));
    gazeTargetY_ = static_cast<int16_t>(random(-8, -3));
  } else {
    // Downward glances are deliberately uncommon and remain within the canvas.
    gazeTargetX_ = static_cast<int16_t>(random(-6, 7));
    gazeTargetY_ = static_cast<int16_t>(random(4, 8));
  }
  autonomousGazePhase_ = AutonomousGazePhase::MOVING;
}

void NewoDisplay::updateAutonomousIdleGaze(uint32_t now) {
  switch (autonomousGazePhase_) {
    case AutonomousGazePhase::CHOOSE_TARGET:
      chooseAutonomousGazeTarget();
      return;
    case AutonomousGazePhase::MOVING:
      gazeX_ = easeAutonomousGaze(gazeX_, gazeTargetX_);
      gazeY_ = easeAutonomousGaze(gazeY_, gazeTargetY_);
      if (gazeX_ != gazeTargetX_ || gazeY_ != gazeTargetY_) return;
      {
        const long durationChoice = random(100);
        const uint32_t fixationMs = durationChoice < 15 ? static_cast<uint32_t>(random(350, 701))
                                    : durationChoice < 25 ? static_cast<uint32_t>(random(2'200, 3'501))
                                                          : static_cast<uint32_t>(random(700, 2'201));
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
    happy = faceStyle_ == NewoFaceStyle::HAPPY || faceStyle_ == NewoFaceStyle::LAUGH;
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
  if (blinkPhase_ == BlinkPhase::OPEN && !winkActive_) {
    if (winkStyle && static_cast<int32_t>(now - nextWinkMs_) >= 0) {
      winkActive_ = true;
      winkStartedMs_ = now;
    } else if (static_cast<int32_t>(now - nextBlinkMs_) >= 0) {
      // Autoblink always animates both eyes; a pending wink waits for it to finish.
      blinkPhase_ = BlinkPhase::HALF_CLOSED;
      blinkFramesRemaining_ = 1;
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
  if (mode_ == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::CURIOUS) {
    // RoboEyes curiosity is directional: the outer eye grows vertically when
    // the gaze reaches an edge rather than merely widening both eyes.
    if (gazeX_ < -7) curiousLeftLift = 8;
    if (gazeX_ > 7) curiousRightLift = 8;
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
      if (faceStyle_ == NewoFaceStyle::WINK_LEFT) {
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
        blinkFramesRemaining_ = verificationBlink_ ? 2 : 1;
      } else if (blinkPhase_ == BlinkPhase::CLOSED) {
        blinkPhase_ = BlinkPhase::HALF_OPEN;
        blinkFramesRemaining_ = 1;
      } else {
        blinkPhase_ = BlinkPhase::OPEN;
        verificationBlink_ = false;
        nextBlinkMs_ = now + static_cast<uint32_t>(random(2'500, 5'501));
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
