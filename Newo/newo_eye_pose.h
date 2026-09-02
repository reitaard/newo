#pragma once

#include <Arduino.h>

// Final eye geometry is expressed as a compact pose. Behaviors and expressions
// choose poses; the renderer consumes only the resolved pose plus gaze/blink.
enum class NewoEyeEasing : uint8_t { LINEAR, EASE_OUT, EASE_IN_OUT };

struct NewoEyePose {
  int16_t leftWidth = 60;
  int16_t rightWidth = 60;
  int16_t leftHeight = 36;
  int16_t rightHeight = 36;
  int16_t gap = 22;
  int16_t leftYOffset = 0;
  int16_t rightYOffset = 0;
  int16_t leftTopCut = 0;
  int16_t rightTopCut = 0;
  int16_t leftBottomCut = 0;
  int16_t rightBottomCut = 0;
  uint8_t openness = 100;
};

class NewoEyePoseEngine {
 public:
  NewoEyePoseEngine();

  void reset(const NewoEyePose& pose, uint32_t now);
  void transitionTo(const NewoEyePose& pose, uint32_t now, uint16_t durationMs,
                    NewoEyeEasing easing = NewoEyeEasing::EASE_IN_OUT);
  const NewoEyePose& update(uint32_t now);
  const NewoEyePose& current() const { return current_; }
  bool active() const { return active_; }

  // Blend an expression against neutral without creating separate weak/strong
  // face variants. Intensity is always bounded 0..100.
  static NewoEyePose blend(const NewoEyePose& neutral, const NewoEyePose& expression,
                           uint8_t intensity);

 private:
  static bool equal(const NewoEyePose& a, const NewoEyePose& b);
  static int16_t interpolate(int16_t from, int16_t to, uint16_t permille);
  static uint16_t easedPermille(NewoEyeEasing easing, uint16_t permille);

  NewoEyePose current_{};
  NewoEyePose origin_{};
  NewoEyePose target_{};
  uint32_t startedMs_ = 0;
  uint16_t durationMs_ = 0;
  NewoEyeEasing easing_ = NewoEyeEasing::EASE_IN_OUT;
  bool active_ = false;
};
