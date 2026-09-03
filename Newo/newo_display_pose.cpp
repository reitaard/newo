#include "newo_display.h"

#include <Fonts/FreeSans9pt7b.h>
#include <cmath>

namespace {
int16_t clampCut(int16_t value, int16_t height) {
  const int16_t magnitude = value < 0 ? -value : value;
  const int16_t maximum = height / 2;
  return magnitude > maximum ? maximum : magnitude;
}
}  // namespace

NewoEyePose NewoDisplay::resolveManualEyePose(NewoFaceStyle style) const {
  NewoEyePose pose;
  switch (style) {
    case NewoFaceStyle::HAPPY:
      pose.leftWidth = pose.rightWidth = 64;
      pose.leftHeight = pose.rightHeight = 39;
      pose.gap = 20;
      pose.leftBottomCut = pose.rightBottomCut = 16;
      break;
    case NewoFaceStyle::ANGRY:
      pose.leftWidth = pose.rightWidth = 62;
      pose.leftHeight = pose.rightHeight = 35;
      pose.gap = 20;
      pose.leftYOffset = pose.rightYOffset = 3;
      pose.leftTopCut = pose.rightTopCut = -17;
      break;
    case NewoFaceStyle::TIRED:
      pose.leftWidth = pose.rightWidth = 62;
      pose.leftHeight = pose.rightHeight = 32;
      pose.leftYOffset = pose.rightYOffset = 3;
      pose.leftTopCut = pose.rightTopCut = 8;
      break;
    case NewoFaceStyle::CURIOUS: {
      // Curiosity is directional: the eye toward attention grows while the
      // opposite eye contracts. Pose and gaze remain separate inputs.
      const int16_t direction = gazeTargetX_ < -7 ? -1 : gazeTargetX_ > 7 ? 1 :
                                (gazeX_ < 0 ? -1 : 1);
      pose.gap = 20;
      if (direction > 0) {
        pose.leftWidth = 50;
        pose.leftHeight = 30;
        pose.rightWidth = 68;
        pose.rightHeight = 50;
        pose.leftYOffset = 2;
        pose.rightYOffset = -1;
      } else {
        pose.leftWidth = 68;
        pose.leftHeight = 50;
        pose.rightWidth = 50;
        pose.rightHeight = 30;
        pose.leftYOffset = -1;
        pose.rightYOffset = 2;
      }
      break;
    }
    case NewoFaceStyle::CONFUSED:
      pose.leftWidth = 55;
      pose.rightWidth = 65;
      pose.leftHeight = 35;
      pose.rightHeight = 38;
      pose.leftYOffset = 2;
      pose.rightYOffset = -2;
      break;
    case NewoFaceStyle::LAUGH:
      pose.leftWidth = pose.rightWidth = 67;
      pose.leftHeight = pose.rightHeight = 33;
      pose.gap = 18;
      pose.leftYOffset = pose.rightYOffset = 2;
      pose.leftBottomCut = pose.rightBottomCut = 18;
      break;
    case NewoFaceStyle::SWEAT:
      pose.leftWidth = 61;
      pose.rightWidth = 55;
      pose.gap = 23;
      break;
    case NewoFaceStyle::CYCLOPS:
      pose.leftWidth = 78;
      pose.rightWidth = 0;
      pose.leftHeight = pose.rightHeight = 43;
      pose.gap = 0;
      break;
    case NewoFaceStyle::CLOSED:
      pose.leftWidth = pose.rightWidth = 58;
      pose.leftHeight = pose.rightHeight = 12;
      pose.gap = 24;
      pose.openness = 0;
      pose.closureStyle = NewoEyeClosureStyle::CURVED;
      break;
    case NewoFaceStyle::DETACHED:
      // The old CLOSED face is intentionally preserved as its own visual.
      pose.leftHeight = pose.rightHeight = 4;
      break;
    case NewoFaceStyle::SLEEPING:
      pose.leftWidth = pose.rightWidth = 58;
      pose.leftHeight = pose.rightHeight = 12;
      pose.gap = 24;
      pose.leftYOffset = pose.rightYOffset = 4;
      pose.openness = 0;
      pose.closureStyle = NewoEyeClosureStyle::CURVED;
      break;
    case NewoFaceStyle::SKEPTICAL: {
      // Skepticism borrows curiosity's directional pose idea: the eye toward
      // attention is large and fully rounded, while the opposite eye is a
      // smaller sharp squint. Crossing gaze direction swaps those roles.
      const int16_t direction = gazeTargetX_ < -5 ? -1 : gazeTargetX_ > 5 ? 1 :
                                (gazeX_ < 0 ? -1 : 1);
      pose.gap = 22;
      if (direction > 0) {
        pose.leftWidth = 50;
        pose.leftHeight = 24;
        pose.leftYOffset = -4;
        pose.leftTopCut = -12;
        pose.rightWidth = 62;
        pose.rightHeight = 38;
      } else {
        pose.leftWidth = 62;
        pose.leftHeight = 38;
        pose.rightWidth = 50;
        pose.rightHeight = 24;
        pose.rightYOffset = -4;
        pose.rightTopCut = -12;
      }
      break;
    }
    case NewoFaceStyle::SURPRISED:
      pose.leftWidth = pose.rightWidth = 54;
      pose.leftHeight = pose.rightHeight = 54;
      pose.gap = 28;
      break;
    case NewoFaceStyle::SLEEPY:
      pose.leftWidth = pose.rightWidth = 62;
      pose.leftHeight = pose.rightHeight = 20;
      pose.leftYOffset = pose.rightYOffset = 5;
      pose.leftTopCut = pose.rightTopCut = 5;
      pose.openness = 72;
      break;
    default:
      break;
  }
  return pose;
}

NewoEyePose NewoDisplay::resolveAutonomousEyePose(uint32_t) const {
  const NewoEyePose neutral;
  NewoEyePose target;
  switch (autonomousExpression_) {
    case AutonomousExpression::CURIOUS: target = resolveManualEyePose(NewoFaceStyle::CURIOUS); break;
    case AutonomousExpression::HAPPY: target = resolveManualEyePose(NewoFaceStyle::HAPPY); break;
    case AutonomousExpression::TIRED: target = resolveManualEyePose(NewoFaceStyle::TIRED); break;
    case AutonomousExpression::SLEEPY: target = resolveManualEyePose(NewoFaceStyle::SLEEPY); break;
    case AutonomousExpression::SURPRISED: target = resolveManualEyePose(NewoFaceStyle::SURPRISED); break;
    case AutonomousExpression::CONFUSED: target = resolveManualEyePose(NewoFaceStyle::CONFUSED); break;
    case AutonomousExpression::SLEEPING: target = resolveManualEyePose(NewoFaceStyle::SLEEPING); break;
    case AutonomousExpression::NONE: return neutral;
  }
  return NewoEyePoseEngine::blend(neutral, target, autonomousExpressionIntensity_);
}

NewoEyePose NewoDisplay::resolveEyePose(uint32_t now, NewoDisplayMode mode) const {
  if (mode == NewoDisplayMode::IDLE) {
    if (autoFaceEnabled_) return resolveAutonomousEyePose(now);
    return resolveManualEyePose(faceStyle_);
  }

  NewoEyePose pose;
  switch (mode) {
    case NewoDisplayMode::LISTENING:
      pose.leftWidth = pose.rightWidth = 69;
      pose.leftHeight = pose.rightHeight = 42;
      pose.gap = 17;
      break;
    case NewoDisplayMode::THINKING:
      pose.leftWidth = 56;
      pose.rightWidth = 62;
      pose.leftHeight = pose.rightHeight = 35;
      pose.leftYOffset = pose.rightYOffset = -2;
      pose.leftTopCut = pose.rightTopCut = 8;
      break;
    case NewoDisplayMode::SPEAKING:
      pose.leftWidth = pose.rightWidth = 62;
      pose.leftHeight = pose.rightHeight = 38;
      pose.gap = 20;
      pose.leftBottomCut = pose.rightBottomCut = 14;
      break;
    case NewoDisplayMode::ERROR:
      pose.leftWidth = pose.rightWidth = 62;
      pose.leftHeight = pose.rightHeight = 34;
      pose.gap = 20;
      pose.leftYOffset = pose.rightYOffset = 5;
      pose.leftTopCut = pose.rightTopCut = -17;
      break;
    default:
      break;
  }
  return pose;
}

NewoDisplay::EyeMotionOverlay NewoDisplay::resolveEyeMotionOverlay(uint32_t now, NewoDisplayMode mode) const {
  EyeMotionOverlay overlay;

  const bool sleeping = mode == NewoDisplayMode::IDLE &&
      ((!autoFaceEnabled_ && faceStyle_ == NewoFaceStyle::SLEEPING) ||
       (autoFaceEnabled_ && autonomousExpression_ == AutonomousExpression::SLEEPING));
  const uint32_t periodMs = sleeping ? 6'000 : 3'000;
  const float phase = static_cast<float>(now % periodMs) / static_cast<float>(periodMs) * 6.2831853f;
  const float amplitude = sleeping ? 1.0f : speakerActive_ ? 2.0f : 1.0f;
  overlay.yOffset = static_cast<int16_t>(sinf(phase) * amplitude);

  if (mode == NewoDisplayMode::IDLE && autoFaceEnabled_ && gazeMotion_.expressive()) {
    overlay.leftWidthDelta += 2;
    overlay.rightWidthDelta += 2;
    overlay.leftHeightDelta -= 2;
    overlay.rightHeightDelta -= 2;
  }

  if (mode == NewoDisplayMode::THINKING) {
    if (gazeX_ < -7) overlay.leftWidthDelta += 7;
    if (gazeX_ > 7) overlay.rightWidthDelta += 7;
  }

  if (mode == NewoDisplayMode::ERROR && now - modeStartedMs_ < 520) {
    overlay.xOffset += static_cast<int16_t>(sinf(static_cast<float>(now - modeStartedMs_) * 0.075f) * 4.0f);
  }

  // Autonomous motion follows expression intent rather than peeking back into
  // the behavior episode that produced it. ALERT_CHECK's confused branch is
  // therefore just a CONFUSED expression with its own secondary shake.
  if (mode == NewoDisplayMode::IDLE && autoFaceEnabled_ &&
      autonomousExpression_ == AutonomousExpression::CONFUSED) {
    overlay.xOffset += static_cast<int16_t>(sinf(static_cast<float>(now) * 0.055f) * 6.0f);
  }

  if (mode == NewoDisplayMode::IDLE && !autoFaceEnabled_) {
    // DETACHED preserves the old CLOSED semantics: its slit eyes do not look
    // around. Cancel the generic manual-gaze offset while keeping subtle base
    // breathing motion from the overlay.
    if (faceStyle_ == NewoFaceStyle::DETACHED) {
      overlay.xOffset -= gazeX_;
      overlay.yOffset -= gazeY_;
    }

    const uint32_t styleBurstMs = (now - modeStartedMs_) % 2'200;
    if (faceStyle_ == NewoFaceStyle::CONFUSED && styleBurstMs < 500) {
      overlay.xOffset += static_cast<int16_t>(sinf(static_cast<float>(styleBurstMs) * 0.075f) * 20.0f);
    }
    if (faceStyle_ == NewoFaceStyle::LAUGH && styleBurstMs < 500) {
      overlay.yOffset += static_cast<int16_t>(sinf(static_cast<float>(styleBurstMs) * 0.075f) * 5.0f);
    }
  }

  return overlay;
}

uint16_t NewoDisplay::eyePoseTransitionMs(NewoDisplayMode mode) const {
  if (mode == NewoDisplayMode::ERROR) return 120;
  if (mode == NewoDisplayMode::LISTENING) return 160;
  if (mode == NewoDisplayMode::SPEAKING) return 180;
  if (mode == NewoDisplayMode::THINKING) return 220;
  if (mode != NewoDisplayMode::IDLE || !autoFaceEnabled_) {
    if (faceStyle_ == NewoFaceStyle::SLEEPING || faceStyle_ == NewoFaceStyle::CLOSED) return 420;
    return 280;
  }

  switch (autonomousExpression_) {
    case AutonomousExpression::SURPRISED:
    case AutonomousExpression::CONFUSED: return 160;
    case AutonomousExpression::CURIOUS: return 190;
    case AutonomousExpression::HAPPY: return 260;
    case AutonomousExpression::TIRED:
    case AutonomousExpression::SLEEPY: return 420;
    case AutonomousExpression::SLEEPING: return 520;
    case AutonomousExpression::NONE: return 280;
  }
  return 280;
}

NewoEyeEasing NewoDisplay::eyePoseEasing(NewoDisplayMode mode) const {
  if (mode == NewoDisplayMode::ERROR || mode == NewoDisplayMode::LISTENING) {
    return NewoEyeEasing::EASE_OUT;
  }
  if (mode == NewoDisplayMode::IDLE && autoFaceEnabled_ &&
      (autonomousExpression_ == AutonomousExpression::SURPRISED ||
       autonomousExpression_ == AutonomousExpression::CONFUSED)) {
    return NewoEyeEasing::EASE_OUT;
  }
  return NewoEyeEasing::EASE_IN_OUT;
}

bool NewoDisplay::setSecondaryEffect(NewoSecondaryEffect effect, uint32_t durationMs) {
  if (effect > NewoSecondaryEffect::SWEAT) return false;
  if (effect == NewoSecondaryEffect::NONE) {
    secondaryEffectOverride_ = NewoSecondaryEffect::NONE;
    secondaryEffectStartedMs_ = 0;
    secondaryEffectUntilMs_ = 0;
    nextFaceFrameMs_ = 0;
    dirty_ = true;
    return true;
  }
  if (durationMs < 500 || durationMs > 15'000) return false;
  const uint32_t now = millis();
  secondaryEffectOverride_ = effect;
  secondaryEffectStartedMs_ = now;
  secondaryEffectUntilMs_ = now + durationMs;
  nextFaceFrameMs_ = 0;
  dirty_ = true;
  return true;
}

NewoSecondaryEffect NewoDisplay::secondaryEffectFor(uint32_t now, NewoDisplayMode mode) const {
  // Operational states own the face completely; decorative effects never fight
  // LISTENING / THINKING / SPEAKING / ERROR or the message/eco screens.
  if (mode != NewoDisplayMode::IDLE) return NewoSecondaryEffect::NONE;

  // Explicit Telegram/manual requests always outrank autonomous composition.
  if (secondaryEffectOverride_ != NewoSecondaryEffect::NONE && secondaryEffectUntilMs_ != 0 &&
      static_cast<int32_t>(now - secondaryEffectUntilMs_) < 0) {
    return secondaryEffectOverride_;
  }

  if (autoFaceEnabled_ && autonomousPresentation_.effect != NewoSecondaryEffect::NONE &&
      autonomousEffectUntilMs_ != 0 && static_cast<int32_t>(now - autonomousEffectUntilMs_) < 0) {
    return autonomousPresentation_.effect;
  }

  // Safety fallback preserves the already-approved sleeping behavior even if a
  // future behavior path forgets to request the presentation cue explicitly.
  if (autoFaceEnabled_) {
    return autonomousExpression_ == AutonomousExpression::SLEEPING ? NewoSecondaryEffect::ZZZ
                                                                    : NewoSecondaryEffect::NONE;
  }
  if (faceStyle_ == NewoFaceStyle::SLEEPING) return NewoSecondaryEffect::ZZZ;
  if (faceStyle_ == NewoFaceStyle::SWEAT) return NewoSecondaryEffect::SWEAT;
  return NewoSecondaryEffect::NONE;
}

bool NewoDisplay::setFaceCaption(NewoFaceCaption caption, uint32_t durationMs) {
  if (caption > NewoFaceCaption::HEY) return false;
  if (caption == NewoFaceCaption::NONE) {
    faceCaptionOverride_ = NewoFaceCaption::NONE;
    faceCaptionStartedMs_ = 0;
    faceCaptionUntilMs_ = 0;
    nextFaceFrameMs_ = 0;
    dirty_ = true;
    return true;
  }
  if (durationMs < 500 || durationMs > 8'000) return false;
  const uint32_t now = millis();
  faceCaptionOverride_ = caption;
  faceCaptionStartedMs_ = now;
  faceCaptionUntilMs_ = now + durationMs;
  nextFaceFrameMs_ = 0;
  dirty_ = true;
  return true;
}

NewoFaceCaption NewoDisplay::faceCaptionFor(uint32_t now, NewoDisplayMode mode) const {
  // Captions are decorative reactions. Operational contexts own the lower face
  // area completely and suppress them without mutating either request source.
  if (mode != NewoDisplayMode::IDLE) return NewoFaceCaption::NONE;

  // Explicit Telegram/manual requests always outrank autonomous composition.
  if (faceCaptionOverride_ != NewoFaceCaption::NONE && faceCaptionUntilMs_ != 0 &&
      static_cast<int32_t>(now - faceCaptionUntilMs_) < 0) {
    return faceCaptionOverride_;
  }

  if (autoFaceEnabled_ && autonomousPresentation_.caption != NewoFaceCaption::NONE &&
      autonomousCaptionUntilMs_ != 0 && static_cast<int32_t>(now - autonomousCaptionUntilMs_) < 0) {
    return autonomousPresentation_.caption;
  }
  return NewoFaceCaption::NONE;
}

const char* NewoDisplay::faceCaptionText(NewoFaceCaption caption) {
  switch (caption) {
    case NewoFaceCaption::HUH: return "Huh?";
    case NewoFaceCaption::WOAH: return "Woah!";
    case NewoFaceCaption::HMM: return "Hmm...";
    case NewoFaceCaption::HEY: return "Hey!";
    case NewoFaceCaption::NONE: return "";
  }
  return "";
}

void NewoDisplay::drawFaceCaption(uint32_t now, NewoFaceCaption caption) {
  constexpr int16_t kCaptionX = 58;
  constexpr int16_t kCaptionY = 128;
  constexpr int16_t kCaptionW = 124;
  constexpr int16_t kCaptionH = 28;
  constexpr int16_t kCaptionBaseline = 150;

  // This band sits below the 200x82 eye canvas (ending at screen Y=122) and
  // above the existing activity strip (screen Y=160..182). It is untouched
  // during ordinary idle frames, so captions add no steady-state SPI traffic.
  if (caption == NewoFaceCaption::NONE) {
    if (!faceCaptionRegionVisible_) return;
    display_.fillRect(kCaptionX, kCaptionY, kCaptionW, kCaptionH, ST77XX_BLACK);
    faceCaptionRegionVisible_ = false;
    return;
  }

  faceCaptionRegionVisible_ = true;
  display_.fillRect(kCaptionX, kCaptionY, kCaptionW, kCaptionH, ST77XX_BLACK);
  const char* text = faceCaptionText(caption);
  int16_t baseline = kCaptionBaseline;
  const bool manualCaptionActive = faceCaptionOverride_ == caption && faceCaptionUntilMs_ != 0 &&
      static_cast<int32_t>(now - faceCaptionUntilMs_) < 0;
  const bool autonomousCaptionActive = autoFaceEnabled_ && autonomousPresentation_.caption == caption &&
      autonomousCaptionUntilMs_ != 0 && static_cast<int32_t>(now - autonomousCaptionUntilMs_) < 0;
  const uint32_t captionStartedMs = manualCaptionActive ? faceCaptionStartedMs_
      : autonomousCaptionActive ? autonomousPresentationStartedMs_ : 0;
  if (captionStartedMs != 0) {
    const uint32_t elapsed = now - captionStartedMs;
    if (elapsed < 160) baseline = static_cast<int16_t>(baseline + 3 - (elapsed * 3 / 160));
  }

  display_.setFont(&FreeSans9pt7b);
  display_.setTextColor(ST77XX_WHITE);
  int16_t x1, y1;
  uint16_t width, height;
  display_.getTextBounds(text, 0, baseline, &x1, &y1, &width, &height);
  const int16_t cursorX = static_cast<int16_t>(kCaptionX + (kCaptionW - static_cast<int16_t>(width)) / 2);
  display_.setCursor(cursorX, baseline);
  display_.print(text);
  display_.setFont(nullptr);
}

void NewoDisplay::applyResolvedPoseCuts(int16_t leftX, int16_t rightX, int16_t leftY, int16_t rightY,
                                        int16_t leftW, int16_t rightW, int16_t leftHeight,
                                        int16_t rightHeight, const NewoEyePose& pose) {
  const auto applyTopCut = [this](int16_t x, int16_t y, int16_t width, int16_t height,
                                  int16_t signedCut, bool leftEye) {
    if (signedCut == 0 || height < 8) return;
    const int16_t cut = clampCut(signedCut, height);
    const bool outerEdge = signedCut > 0;
    const bool lowerLeft = leftEye ? outerEdge : !outerEdge;
    if (lowerLeft) {
      eyeCanvas_.fillTriangle(x, y, x + width, y, x, y + cut, 0);
    } else {
      eyeCanvas_.fillTriangle(x, y, x + width, y, x + width, y + cut, 0);
    }
  };

  const auto applyBottomCut = [this](int16_t x, int16_t y, int16_t width, int16_t height,
                                     int16_t cut) {
    if (cut <= 0 || height < 8) return;
    const int16_t radius = height / 2 + (cut / 4 > 2 ? cut / 4 : 3);
    const int16_t centerY = y + height + 4;
    eyeCanvas_.fillCircle(x + width / 2, centerY, radius, 0);
  };

  applyTopCut(leftX, leftY, leftW, leftHeight, pose.leftTopCut, true);
  applyTopCut(rightX, rightY, rightW, rightHeight, pose.rightTopCut, false);
  applyBottomCut(leftX, leftY, leftW, leftHeight, pose.leftBottomCut);
  applyBottomCut(rightX, rightY, rightW, rightHeight, pose.rightBottomCut);
}

void NewoDisplay::drawClosedEyeCurve(int16_t x, int16_t y, int16_t width) {
  const int16_t middle = x + width / 2;
  eyeCanvas_.drawLine(x, y, middle, y + 3, 1);
  eyeCanvas_.drawLine(middle, y + 3, x + width, y, 1);
  eyeCanvas_.drawLine(x, y + 1, middle, y + 4, 1);
  eyeCanvas_.drawLine(middle, y + 4, x + width, y + 1, 1);
}

void NewoDisplay::drawZ(int16_t x, int16_t y, int16_t size) {
  if (size < 5) return;
  // Two-pixel strokes remain legible after the 1-bit canvas is transferred to
  // the physical TFT; the former 1px 5/7/9px glyphs looked fragmented.
  eyeCanvas_.fillRect(x, y, size + 1, 2, 1);
  eyeCanvas_.drawLine(x + size, y + 1, x, y + size, 1);
  eyeCanvas_.drawLine(x + size - 1, y + 1, x, y + size - 1, 1);
  eyeCanvas_.fillRect(x, y + size - 1, size + 1, 2, 1);
}

void NewoDisplay::drawSecondaryEffect(uint32_t now, NewoSecondaryEffect effect) {
  // Caption state is independent from secondary-effect state. This renderer
  // hook services both bounded decorative layers before the eye canvas blit.
  drawFaceCaption(now, faceCaptionFor(now, effectiveMode(now)));

  const bool manualEffectActive = secondaryEffectOverride_ == effect && secondaryEffectUntilMs_ != 0 &&
      static_cast<int32_t>(now - secondaryEffectUntilMs_) < 0;
  const bool autonomousEffectActive = autoFaceEnabled_ && autonomousPresentation_.effect == effect &&
      autonomousEffectUntilMs_ != 0 && static_cast<int32_t>(now - autonomousEffectUntilMs_) < 0;
  const uint32_t effectStartedMs = manualEffectActive ? secondaryEffectStartedMs_
      : autonomousEffectActive ? autonomousPresentationStartedMs_ : 0;
  const uint32_t effectNow = effectStartedMs != 0 ? now - effectStartedMs : now;

  if (effect == NewoSecondaryEffect::ZZZ) {
    // Two readable glyphs work better than three tiny ones on the 200x82 eye
    // canvas. They are staggered, safely inset, and drift only a few pixels so
    // neither glyph approaches the top/right edge.
    const uint16_t cycle = static_cast<uint16_t>(effectNow % 2'400);
    for (uint8_t index = 0; index < 2; ++index) {
      const uint16_t local = static_cast<uint16_t>((cycle + index * 1'200) % 2'400);
      if (local >= 1'700) continue;
      const int16_t rise = static_cast<int16_t>(local / 210);
      const int16_t drift = static_cast<int16_t>(local / 550);
      const int16_t size = index == 0 ? 7 : 10;
      const int16_t baseX = index == 0 ? 145 : 165;
      const int16_t baseY = index == 0 ? 30 : 22;
      drawZ(static_cast<int16_t>(baseX + drift), static_cast<int16_t>(baseY - rise), size);
    }
    return;
  }

  const auto drawQuestion = [this](int16_t x, int16_t y) {
    // 2px procedural strokes: compact enough for the top-right margin but
    // thick enough to survive the 1-bit canvas -> TFT transfer.
    eyeCanvas_.fillRect(x + 2, y, 6, 2, 1);
    eyeCanvas_.fillRect(x + 7, y + 2, 2, 4, 1);
    eyeCanvas_.drawLine(x + 7, y + 5, x + 4, y + 8, 1);
    eyeCanvas_.drawLine(x + 6, y + 5, x + 3, y + 8, 1);
    eyeCanvas_.fillRect(x + 3, y + 8, 2, 3, 1);
    eyeCanvas_.fillRect(x + 3, y + 13, 2, 2, 1);
  };
  const auto drawExclamation = [this](int16_t x, int16_t y) {
    eyeCanvas_.fillRect(x, y, 3, 8, 1);
    eyeCanvas_.fillRect(x, y + 11, 3, 3, 1);
  };

  if (effect == NewoSecondaryEffect::QUESTION) {
    const uint16_t local = static_cast<uint16_t>(effectNow % 1'800);
    if (local >= 1'450) return;
    int16_t rise = static_cast<int16_t>(local / 300);
    if (rise > 4) rise = 4;
    drawQuestion(177, static_cast<int16_t>(6 - rise));
    return;
  }

  if (effect == NewoSecondaryEffect::EXCLAMATION) {
    const uint16_t local = static_cast<uint16_t>(effectNow % 1'650);
    if (local >= 1'250) return;
    int16_t rise = static_cast<int16_t>(local / 60);
    if (rise > 4) rise = 4;
    drawExclamation(183, static_cast<int16_t>(7 - rise));
    return;
  }

  if (effect == NewoSecondaryEffect::SURPRISE_MARK) {
    const uint16_t local = static_cast<uint16_t>(effectNow % 1'750);
    if (local >= 1'300) return;
    int16_t rise = static_cast<int16_t>(local / 70);
    if (rise > 4) rise = 4;
    const int16_t y = static_cast<int16_t>(7 - rise);
    drawExclamation(166, y);
    drawQuestion(174, static_cast<int16_t>(y - 1));
    return;
  }

  if (effect == NewoSecondaryEffect::ELLIPSIS) {
    const uint16_t local = static_cast<uint16_t>(effectNow % 1'800);
    uint8_t count = 0;
    if (local < 300) count = 1;
    else if (local < 600) count = 2;
    else if (local < 1'450) count = 3;
    for (uint8_t index = 0; index < count; ++index) {
      eyeCanvas_.fillCircle(static_cast<int16_t>(169 + index * 7), 9, 2, 1);
    }
    return;
  }

  if (effect == NewoSecondaryEffect::SWEAT) {
    for (uint8_t drop = 0; drop < 3; ++drop) {
      const uint16_t phase = static_cast<uint16_t>((effectNow / 9 + drop * 31) % 100);
      const int16_t dropX = 42 + drop * 58;
      const int16_t dropY = 3 + phase / 4;
      const int16_t radius = phase < 50 ? 1 + phase / 25 : 1 + (99 - phase) / 25;
      eyeCanvas_.fillCircle(dropX, dropY + radius, radius, 1);
      eyeCanvas_.fillTriangle(dropX, dropY - radius - 1, dropX - radius, dropY + radius,
                              dropX + radius, dropY + radius, 1);
    }
  }
}
