#include "newo_eye_pose.h"

namespace {
uint8_t clampIntensity(uint8_t intensity) {
  return intensity > 100 ? 100 : intensity;
}

NewoEyeClosureStyle interpolateClosureStyle(NewoEyeClosureStyle from, NewoEyeClosureStyle to,
                                             uint16_t permille) {
  if (from == to) return to;
  // Closing should keep the filled-eye silhouette until it is almost shut;
  // waking should return to filled geometry early so openness can expand it
  // smoothly. This avoids a visible mid-transition shape pop.
  if (to == NewoEyeClosureStyle::CURVED) {
    return permille < 850 ? from : to;
  }
  return permille < 150 ? from : to;
}
}  // namespace

NewoEyePoseEngine::NewoEyePoseEngine() {
  reset(NewoEyePose{}, 0);
}

void NewoEyePoseEngine::reset(const NewoEyePose& pose, uint32_t now) {
  current_ = pose;
  origin_ = pose;
  target_ = pose;
  startedMs_ = now;
  durationMs_ = 0;
  active_ = false;
}

void NewoEyePoseEngine::transitionTo(const NewoEyePose& pose, uint32_t now, uint16_t durationMs,
                                    NewoEyeEasing easing) {
  update(now);
  // The display resolves its target every frame. Do not restart an in-flight
  // transition just because the same target was requested again at 20 FPS.
  if (active_ && equal(target_, pose)) return;
  if (equal(current_, pose) || durationMs == 0) {
    reset(pose, now);
    return;
  }
  origin_ = current_;
  target_ = pose;
  startedMs_ = now;
  durationMs_ = durationMs;
  easing_ = easing;
  active_ = true;
}

const NewoEyePose& NewoEyePoseEngine::update(uint32_t now) {
  if (!active_) return current_;
  const uint32_t elapsed = now - startedMs_;
  if (elapsed >= durationMs_) {
    current_ = target_;
    active_ = false;
    return current_;
  }

  const uint16_t raw = static_cast<uint16_t>((elapsed * 1000UL) / durationMs_);
  const uint16_t t = easedPermille(easing_, raw);
  current_.leftWidth = interpolate(origin_.leftWidth, target_.leftWidth, t);
  current_.rightWidth = interpolate(origin_.rightWidth, target_.rightWidth, t);
  current_.leftHeight = interpolate(origin_.leftHeight, target_.leftHeight, t);
  current_.rightHeight = interpolate(origin_.rightHeight, target_.rightHeight, t);
  current_.gap = interpolate(origin_.gap, target_.gap, t);
  current_.leftYOffset = interpolate(origin_.leftYOffset, target_.leftYOffset, t);
  current_.rightYOffset = interpolate(origin_.rightYOffset, target_.rightYOffset, t);
  current_.leftTopCut = interpolate(origin_.leftTopCut, target_.leftTopCut, t);
  current_.rightTopCut = interpolate(origin_.rightTopCut, target_.rightTopCut, t);
  current_.leftBottomCut = interpolate(origin_.leftBottomCut, target_.leftBottomCut, t);
  current_.rightBottomCut = interpolate(origin_.rightBottomCut, target_.rightBottomCut, t);
  current_.openness = static_cast<uint8_t>(interpolate(origin_.openness, target_.openness, t));
  current_.closureStyle = interpolateClosureStyle(origin_.closureStyle, target_.closureStyle, t);
  return current_;
}

NewoEyePose NewoEyePoseEngine::blend(const NewoEyePose& neutral, const NewoEyePose& expression,
                                     uint8_t intensity) {
  const uint16_t t = static_cast<uint16_t>(clampIntensity(intensity)) * 10U;
  NewoEyePose out;
  out.leftWidth = interpolate(neutral.leftWidth, expression.leftWidth, t);
  out.rightWidth = interpolate(neutral.rightWidth, expression.rightWidth, t);
  out.leftHeight = interpolate(neutral.leftHeight, expression.leftHeight, t);
  out.rightHeight = interpolate(neutral.rightHeight, expression.rightHeight, t);
  out.gap = interpolate(neutral.gap, expression.gap, t);
  out.leftYOffset = interpolate(neutral.leftYOffset, expression.leftYOffset, t);
  out.rightYOffset = interpolate(neutral.rightYOffset, expression.rightYOffset, t);
  out.leftTopCut = interpolate(neutral.leftTopCut, expression.leftTopCut, t);
  out.rightTopCut = interpolate(neutral.rightTopCut, expression.rightTopCut, t);
  out.leftBottomCut = interpolate(neutral.leftBottomCut, expression.leftBottomCut, t);
  out.rightBottomCut = interpolate(neutral.rightBottomCut, expression.rightBottomCut, t);
  out.openness = static_cast<uint8_t>(interpolate(neutral.openness, expression.openness, t));
  out.closureStyle = interpolateClosureStyle(neutral.closureStyle, expression.closureStyle, t);
  return out;
}

bool NewoEyePoseEngine::equal(const NewoEyePose& a, const NewoEyePose& b) {
  return a.leftWidth == b.leftWidth && a.rightWidth == b.rightWidth &&
         a.leftHeight == b.leftHeight && a.rightHeight == b.rightHeight && a.gap == b.gap &&
         a.leftYOffset == b.leftYOffset && a.rightYOffset == b.rightYOffset &&
         a.leftTopCut == b.leftTopCut && a.rightTopCut == b.rightTopCut &&
         a.leftBottomCut == b.leftBottomCut && a.rightBottomCut == b.rightBottomCut &&
         a.openness == b.openness && a.closureStyle == b.closureStyle;
}

int16_t NewoEyePoseEngine::interpolate(int16_t from, int16_t to, uint16_t permille) {
  if (permille >= 1000) return to;
  const int32_t delta = static_cast<int32_t>(to) - from;
  return static_cast<int16_t>(from + (delta * permille + (delta >= 0 ? 500 : -500)) / 1000);
}

uint16_t NewoEyePoseEngine::easedPermille(NewoEyeEasing easing, uint16_t permille) {
  if (permille >= 1000) return 1000;
  switch (easing) {
    case NewoEyeEasing::LINEAR:
      return permille;
    case NewoEyeEasing::EASE_OUT: {
      const uint32_t inverse = 1000U - permille;
      return static_cast<uint16_t>(1000U - (inverse * inverse) / 1000U);
    }
    case NewoEyeEasing::EASE_IN_OUT:
      if (permille < 500) {
        return static_cast<uint16_t>((2UL * permille * permille) / 1000UL);
      } else {
        const uint32_t inverse = 1000U - permille;
        return static_cast<uint16_t>(1000U - (2UL * inverse * inverse) / 1000UL);
      }
  }
  return permille;
}
