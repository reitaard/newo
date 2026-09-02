#include "newo_gaze_motion.h"

namespace {
int8_t signOf(int16_t value) {
  return value > 0 ? 1 : value < 0 ? -1 : 0;
}
}  // namespace

void NewoGazeMotion::reset(int16_t x, int16_t y) {
  stage_ = Stage::IDLE;
  startX_ = destinationX_ = activeTargetX_ = x;
  startY_ = destinationY_ = activeTargetY_ = y;
  directionX_ = 0;
  directionY_ = 0;
  expressive_ = false;
  hasOvershoot_ = false;
}

void NewoGazeMotion::start(int16_t currentX, int16_t currentY, int16_t destinationX, int16_t destinationY,
                           int16_t hardX, int16_t hardY, bool expressive) {
  startX_ = currentX;
  startY_ = currentY;
  hardX_ = hardX;
  hardY_ = hardY;
  destinationX_ = clamp(destinationX, hardX_);
  destinationY_ = clamp(destinationY, hardY_);
  const int16_t deltaX = destinationX_ - currentX;
  const int16_t deltaY = destinationY_ - currentY;
  directionX_ = signOf(deltaX);
  directionY_ = signOf(deltaY);
  const int16_t absX = deltaX < 0 ? -deltaX : deltaX;
  const int16_t absY = deltaY < 0 ? -deltaY : deltaY;
  expressive_ = expressive && (absX >= 12 || absY >= 7);
  hasOvershoot_ = false;

  if (!expressive_) {
    stage_ = Stage::TRAVEL;
    activeTargetX_ = destinationX_;
    activeTargetY_ = destinationY_;
    return;
  }

  // One tiny counter-motion frame gives a large gaze an intention cue before
  // the saccade. It stays inside the same approved gaze envelope.
  activeTargetX_ = clamp(static_cast<int16_t>(currentX - directionX_ * 2), hardX_);
  activeTargetY_ = clamp(static_cast<int16_t>(currentY - directionY_), hardY_);
  if (activeTargetX_ == currentX && activeTargetY_ == currentY) {
    setTravelTarget();
    return;
  }
  stage_ = Stage::ANTICIPATE;
}

bool NewoGazeMotion::update(int16_t& x, int16_t& y) {
  if (stage_ == Stage::IDLE) return true;
  x = ease(x, activeTargetX_);
  y = ease(y, activeTargetY_);
  if (x != activeTargetX_ || y != activeTargetY_) return false;

  switch (stage_) {
    case Stage::ANTICIPATE:
      setTravelTarget();
      return false;
    case Stage::TRAVEL:
      if (hasOvershoot_) {
        stage_ = Stage::SETTLE;
        activeTargetX_ = destinationX_;
        activeTargetY_ = destinationY_;
        return false;
      }
      stage_ = Stage::IDLE;
      expressive_ = false;
      return true;
    case Stage::SETTLE:
      stage_ = Stage::IDLE;
      expressive_ = false;
      return true;
    case Stage::IDLE:
      return true;
  }
  return true;
}

int16_t NewoGazeMotion::clamp(int16_t value, int16_t limit) {
  if (value < -limit) return static_cast<int16_t>(-limit);
  return value > limit ? limit : value;
}

int16_t NewoGazeMotion::ease(int16_t current, int16_t target) {
  const int16_t delta = target - current;
  const int16_t magnitude = delta < 0 ? -delta : delta;
  if (magnitude < 2) return target;
  int16_t step = magnitude / 2;
  if (step > 8) step = 8;
  if (step < 1) step = 1;
  return current + (delta > 0 ? step : -step);
}

void NewoGazeMotion::setTravelTarget() {
  stage_ = Stage::TRAVEL;
  const int16_t overshootX = clamp(static_cast<int16_t>(destinationX_ + directionX_ * 2), hardX_);
  const int16_t overshootY = clamp(static_cast<int16_t>(destinationY_ + directionY_), hardY_);
  hasOvershoot_ = overshootX != destinationX_ || overshootY != destinationY_;
  activeTargetX_ = hasOvershoot_ ? overshootX : destinationX_;
  activeTargetY_ = hasOvershoot_ ? overshootY : destinationY_;
}
