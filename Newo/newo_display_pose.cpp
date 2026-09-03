#include "newo_display.h"

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

NewoDisplay::SecondaryEffect NewoDisplay::secondaryEffectFor(uint32_t, NewoDisplayMode mode) const {
  if (mode != NewoDisplayMode::IDLE) return SecondaryEffect::NONE;
  if (autoFaceEnabled_) {
    return autonomousExpression_ == AutonomousExpression::SLEEPING ? SecondaryEffect::ZZZ : SecondaryEffect::NONE;
  }
  if (faceStyle_ == NewoFaceStyle::SLEEPING) return SecondaryEffect::ZZZ;
  if (faceStyle_ == NewoFaceStyle::SWEAT) return SecondaryEffect::SWEAT;
  return SecondaryEffect::NONE;
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

void NewoDisplay::drawSecondaryEffect(uint32_t now, SecondaryEffect effect) {
  if (effect == SecondaryEffect::ZZZ) {
    // Two readable glyphs work better than three tiny ones on the 200x82 eye
    // canvas. They are staggered, safely inset, and drift only a few pixels so
    // neither glyph approaches the top/right edge.
    const uint16_t cycle = static_cast<uint16_t>(now % 2'400);
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

  if (effect == SecondaryEffect::SWEAT) {
    for (uint8_t drop = 0; drop < 3; ++drop) {
      const uint16_t phase = static_cast<uint16_t>((now / 9 + drop * 31) % 100);
      const int16_t dropX = 42 + drop * 58;
      const int16_t dropY = 3 + phase / 4;
      const int16_t radius = phase < 50 ? 1 + phase / 25 : 1 + (99 - phase) / 25;
      eyeCanvas_.fillCircle(dropX, dropY + radius, radius, 1);
      eyeCanvas_.fillTriangle(dropX, dropY - radius - 1, dropX - radius, dropY + radius,
                              dropX + radius, dropY + radius, 1);
    }
  }
}
