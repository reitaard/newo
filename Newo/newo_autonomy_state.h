#pragma once

#include <stdint.h>

enum class NewoAutonomyEngagement : uint8_t {
  IDLE,
  LISTENING,
  THINKING,
  SPEAKING,
};

enum class NewoInactivityStage : uint8_t {
  ACTIVE,
  RELAXED,
  DROWSY,
};

// Small host-testable character-state engine. It owns only slow semantic state;
// it never selects an expression, gaze target, effect, caption, or renderer path.
class NewoAutonomyState {
 public:
  static constexpr uint8_t kEnergyBaseline = 70;
  static constexpr uint8_t kCuriosityBaseline = 42;
  static constexpr uint8_t kSocialBaseline = 38;
  static constexpr uint8_t kStressBaseline = 5;

  static constexpr uint32_t kInactivityBeforeEnergyDriftMs = 30'000;
  static constexpr uint32_t kRelaxedInactivityMs = 120'000;
  static constexpr uint32_t kDrowsyInactivityMs = 300'000;

  void reset(uint32_t now);
  void noteInteraction(uint32_t now, uint8_t energyGain, uint8_t curiosityGain,
                       uint8_t socialGain);
  void noteError();
  void update(uint32_t now, NewoAutonomyEngagement engagement);

  NewoInactivityStage stage(uint32_t now) const;
  uint32_t inactiveMs(uint32_t now) const { return now - lastInteractionMs_; }

  uint8_t energy() const { return energy_; }
  uint8_t fatigue() const { return static_cast<uint8_t>(100 - energy_); }
  uint8_t curiosity() const { return curiosity_; }
  uint8_t social() const { return social_; }
  uint8_t stress() const { return stress_; }
  uint32_t lastInteractionMs() const { return lastInteractionMs_; }

 private:
  static uint8_t saturatingAdd(uint8_t value, uint8_t amount);
  static uint8_t engagementSocialTarget(NewoAutonomyEngagement engagement);
  static uint8_t engagementEnergyTarget(NewoAutonomyEngagement engagement);
  static uint32_t idleEnergyIntervalMs(NewoInactivityStage stage);
  static uint8_t idleEnergyFloor(NewoInactivityStage stage);

  uint8_t energy_ = kEnergyBaseline;
  uint8_t curiosity_ = kCuriosityBaseline;
  uint8_t social_ = kSocialBaseline;
  uint8_t stress_ = kStressBaseline;
  uint32_t lastInteractionMs_ = 0;
  uint32_t lastEnergyStepMs_ = 0;
};
