#include "newo_display.h"

namespace {
constexpr uint32_t kPoseDrowsyInactivityMs = 300'000;

uint8_t clampPercent(int value) {
  if (value < 0) return 0;
  return value > 100 ? 100 : static_cast<uint8_t>(value);
}

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
      // Curiosity is intentionally asymmetric. The eye toward the gaze grows
      // while the opposite eye contracts, making direction readable at a glance.
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
    case NewoFaceStyle::SKEPTICAL:
      pose.leftWidth = 62;
      pose.leftHeight = 38;
      pose.rightWidth = 50;
      pose.rightHeight = 24;
      pose.gap = 22;
      pose.rightYOffset = -5;
      pose.rightTopCut = -4;
      break;
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

NewoEyePose NewoDisplay::resolveAutonomousEyePose(uint32_t now) const {
  const NewoEyePose neutral;
  switch (autonomousEpisode_) {
    case AutonomousEpisode::CURIOUS_SCAN: {
      NewoEyePose target = resolveManualEyePose(NewoFaceStyle::CURIOUS);
      const int curiosityDelta = curiosity_ > 42 ? static_cast<int>(curiosity_ - 42) : 0;
      return NewoEyePoseEngine::blend(neutral, target, clampPercent(72 + curiosityDelta * 3));
    }
    case AutonomousEpisode::SOCIAL_ATTENTION: {
      const NewoEyePose target = resolveManualEyePose(NewoFaceStyle::HAPPY);
      return NewoEyePoseEngine::blend(neutral, target, clampPercent(55 + static_cast<int>(social_) / 2));
    }
    case AutonomousEpisode::LOW_ENERGY: {
      const bool drowsy = now - lastInteractionMs_ >= kPoseDrowsyInactivityMs;
      const NewoEyePose target = resolveManualEyePose(drowsy ? NewoFaceStyle::SLEEPY : NewoFaceStyle::TIRED);
      const int lowEnergy = energy_ < 70 ? static_cast<int>(70 - energy_) : 0;
      const uint8_t intensity = drowsy ? clampPercent(88 + lowEnergy / 2)
                                       : clampPercent(65 + lowEnergy);
      return NewoEyePoseEngine::blend(neutral, target, intensity);
    }
    case AutonomousEpisode::ALERT_CHECK: {
      const NewoEyePose target = resolveManualEyePose(autonomousEpisodeDirection_ > 0
          ? NewoFaceStyle::SURPRISED : NewoFaceStyle::CONFUSED);
      return NewoEyePoseEngine::blend(neutral, target,
                                     clampPercent(70 + static_cast<int>(stress_) * 2));
    }
    case AutonomousEpisode::WAITING:
      return neutral;
  }
  return neutral;
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

uint16_t NewoDisplay::eyePoseTransitionMs(NewoDisplayMode mode) const {
  if (mode == NewoDisplayMode::ERROR) return 120;
  if (mode == NewoDisplayMode::LISTENING) return 160;
  if (mode == NewoDisplayMode::SPEAKING) return 180;
  if (mode == NewoDisplayMode::THINKING) return 220;
  if (mode != NewoDisplayMode::IDLE || !autoFaceEnabled_) {
    if (faceStyle_ == NewoFaceStyle::SLEEPING || faceStyle_ == NewoFaceStyle::CLOSED) return 420;
    return 280;
  }

  switch (autonomousEpisode_) {
    case AutonomousEpisode::ALERT_CHECK: return 160;
    case AutonomousEpisode::CURIOUS_SCAN: return 190;
    case AutonomousEpisode::SOCIAL_ATTENTION: return 260;
    case AutonomousEpisode::LOW_ENERGY: return 420;
    case AutonomousEpisode::WAITING: return 280;
  }
  return 280;
}

NewoEyeEasing NewoDisplay::eyePoseEasing(NewoDisplayMode mode) const {
  if (mode == NewoDisplayMode::ERROR || mode == NewoDisplayMode::LISTENING) {
    return NewoEyeEasing::EASE_OUT;
  }
  if (mode == NewoDisplayMode::IDLE && autoFaceEnabled_ &&
      autonomousEpisode_ == AutonomousEpisode::ALERT_CHECK) {
    return NewoEyeEasing::EASE_OUT;
  }
  return NewoEyeEasing::EASE_IN_OUT;
}

NewoDisplay::SecondaryEffect NewoDisplay::secondaryEffectFor(uint32_t, NewoDisplayMode mode) const {
  if (mode != NewoDisplayMode::IDLE || autoFaceEnabled_) return SecondaryEffect::NONE;
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
  if (size < 3) return;
  eyeCanvas_.drawLine(x, y, x + size, y, 1);
  eyeCanvas_.drawLine(x + size, y, x, y + size, 1);
  eyeCanvas_.drawLine(x, y + size, x + size, y + size, 1);
}

void NewoDisplay::drawSecondaryEffect(uint32_t now, SecondaryEffect effect) {
  if (effect == SecondaryEffect::ZZZ) {
    const uint16_t cycle = static_cast<uint16_t>(now % 3'000);
    for (uint8_t index = 0; index < 3; ++index) {
      const uint16_t local = static_cast<uint16_t>((cycle + index * 1'000) % 3'000);
      if (local >= 1'850) continue;
      const int16_t rise = static_cast<int16_t>(local / 140);
      const int16_t size = static_cast<int16_t>(5 + index * 2);
      drawZ(static_cast<int16_t>(146 + index * 15),
            static_cast<int16_t>(22 + index * 3 - rise), size);
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
