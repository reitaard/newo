#include "newo_autonomy_state.h"

namespace {
constexpr uint8_t kIdleSocialFloor = 30;
constexpr uint32_t kEngagementEnergyRecoveryMs = 18'000;
}

uint8_t NewoAutonomyState::saturatingAdd(uint8_t value, uint8_t amount) {
  const uint16_t result = static_cast<uint16_t>(value) + amount;
  return result > 100 ? 100 : static_cast<uint8_t>(result);
}

uint8_t NewoAutonomyState::engagementSocialTarget(NewoAutonomyEngagement engagement) {
  switch (engagement) {
    case NewoAutonomyEngagement::SPEAKING: return 82;
    case NewoAutonomyEngagement::LISTENING: return 78;
    case NewoAutonomyEngagement::THINKING: return 68;
    case NewoAutonomyEngagement::IDLE: return kIdleSocialFloor;
  }
  return kIdleSocialFloor;
}

uint8_t NewoAutonomyState::engagementEnergyTarget(NewoAutonomyEngagement engagement) {
  switch (engagement) {
    case NewoAutonomyEngagement::SPEAKING: return 80;
    case NewoAutonomyEngagement::LISTENING: return 78;
    case NewoAutonomyEngagement::THINKING: return 74;
    case NewoAutonomyEngagement::IDLE: return kEnergyBaseline;
  }
  return kEnergyBaseline;
}

uint32_t NewoAutonomyState::idleEnergyIntervalMs(NewoInactivityStage stage) {
  switch (stage) {
    case NewoInactivityStage::ACTIVE: return 36'000;
    case NewoInactivityStage::RELAXED: return 24'000;
    case NewoInactivityStage::DROWSY: return 18'000;
  }
  return 36'000;
}

uint8_t NewoAutonomyState::idleEnergyFloor(NewoInactivityStage stage) {
  switch (stage) {
    case NewoInactivityStage::ACTIVE: return 62;
    case NewoInactivityStage::RELAXED: return 50;
    case NewoInactivityStage::DROWSY: return 35;
  }
  return 35;
}

void NewoAutonomyState::reset(uint32_t now) {
  energy_ = kEnergyBaseline;
  curiosity_ = kCuriosityBaseline;
  social_ = kSocialBaseline;
  stress_ = kStressBaseline;
  lastInteractionMs_ = now;
  lastEnergyStepMs_ = now;
}

void NewoAutonomyState::noteInteraction(uint32_t now, uint8_t energyGain, uint8_t curiosityGain,
                                        uint8_t socialGain) {
  lastInteractionMs_ = now;
  lastEnergyStepMs_ = now;
  energy_ = saturatingAdd(energy_, energyGain);
  curiosity_ = saturatingAdd(curiosity_, curiosityGain);
  social_ = saturatingAdd(social_, socialGain);
  if (stress_ > 0) --stress_;
}

void NewoAutonomyState::noteError() {
  stress_ = saturatingAdd(stress_, 12);
}

NewoInactivityStage NewoAutonomyState::stage(uint32_t now) const {
  const uint32_t inactive = inactiveMs(now);
  if (inactive >= kDrowsyInactivityMs) return NewoInactivityStage::DROWSY;
  if (inactive >= kRelaxedInactivityMs) return NewoInactivityStage::RELAXED;
  return NewoInactivityStage::ACTIVE;
}

void NewoAutonomyState::update(uint32_t now, NewoAutonomyEngagement engagement) {
  if (curiosity_ > kCuriosityBaseline) --curiosity_;
  else if (curiosity_ < kCuriosityBaseline) ++curiosity_;
  if (stress_ > 0) --stress_;

  if (engagement != NewoAutonomyEngagement::IDLE) {
    lastInteractionMs_ = now;
    const uint8_t socialTarget = engagementSocialTarget(engagement);
    if (social_ < socialTarget) {
      const uint8_t gap = static_cast<uint8_t>(socialTarget - social_);
      social_ = static_cast<uint8_t>(social_ + (gap > 1 ? 2 : 1));
    }

    const uint8_t energyTarget = engagementEnergyTarget(engagement);
    if (energy_ < energyTarget && now - lastEnergyStepMs_ >= kEngagementEnergyRecoveryMs) {
      ++energy_;
      lastEnergyStepMs_ = now;
    } else if (energy_ >= energyTarget) {
      lastEnergyStepMs_ = now;
    }
    return;
  }

  if (social_ > kIdleSocialFloor) --social_;

  const uint32_t inactive = inactiveMs(now);
  if (inactive < kInactivityBeforeEnergyDriftMs) {
    lastEnergyStepMs_ = now;
    return;
  }

  const NewoInactivityStage currentStage = stage(now);
  const uint32_t intervalMs = idleEnergyIntervalMs(currentStage);
  if (now - lastEnergyStepMs_ < intervalMs) return;

  const uint8_t floor = idleEnergyFloor(currentStage);
  if (energy_ > floor) --energy_;
  lastEnergyStepMs_ = now;
}
