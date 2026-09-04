#pragma once

#include <stdint.h>

// Gaze destination and gaze motion are separate concerns. Behaviors choose a
// destination; this tiny state machine gives large saccades anticipation and
// bounded follow-through without changing the destination or allocating memory.
class NewoGazeMotion {
 public:
  void reset(int16_t x = 0, int16_t y = 0);
  void start(int16_t currentX, int16_t currentY, int16_t destinationX, int16_t destinationY,
             int16_t hardX, int16_t hardY, bool expressive);
  bool update(int16_t& x, int16_t& y);

  bool active() const { return stage_ != Stage::IDLE; }
  bool expressive() const { return expressive_ && active(); }
  int16_t destinationX() const { return destinationX_; }
  int16_t destinationY() const { return destinationY_; }

 private:
  enum class Stage : uint8_t { IDLE, ANTICIPATE, TRAVEL, SETTLE };

  static int16_t clamp(int16_t value, int16_t limit);
  static int16_t ease(int16_t current, int16_t target);
  void setTravelTarget();

  Stage stage_ = Stage::IDLE;
  int16_t startX_ = 0;
  int16_t startY_ = 0;
  int16_t destinationX_ = 0;
  int16_t destinationY_ = 0;
  int16_t activeTargetX_ = 0;
  int16_t activeTargetY_ = 0;
  int16_t hardX_ = 20;
  int16_t hardY_ = 12;
  int8_t directionX_ = 0;
  int8_t directionY_ = 0;
  bool expressive_ = false;
  bool hasOvershoot_ = false;
};
