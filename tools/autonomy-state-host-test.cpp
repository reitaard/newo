#include <cstdlib>
#include <iostream>

#include "../Newo/newo_autonomy_state.h"

namespace {
void require(bool condition, const char* message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void updateEveryThreeSeconds(NewoAutonomyState& state, uint32_t fromMs, uint32_t toMs,
                             NewoAutonomyEngagement engagement) {
  for (uint32_t now = fromMs; now <= toMs; now += 3'000) state.update(now, engagement);
}

void testResetAndStages() {
  NewoAutonomyState state;
  state.reset(1'000);
  require(state.energy() == 70, "reset energy must match character baseline");
  require(state.curiosity() == 42, "reset curiosity must match baseline");
  require(state.social() == 38, "reset social must match baseline");
  require(state.stress() == 5, "reset stress must match baseline");
  require(state.fatigue() == 30, "fatigue must be derived from energy");
  require(state.stage(120'999) == NewoInactivityStage::ACTIVE,
          "inactivity must remain ACTIVE before two minutes");
  require(state.stage(121'000) == NewoInactivityStage::RELAXED,
          "inactivity must become RELAXED at two minutes");
  require(state.stage(301'000) == NewoInactivityStage::DROWSY,
          "inactivity must become DROWSY at five minutes");
}

void testIdleEnergyLifecycleAndFloor() {
  NewoAutonomyState state;
  state.reset(0);
  updateEveryThreeSeconds(state, 3'000, 120'000, NewoAutonomyEngagement::IDLE);
  require(state.energy() < 70 && state.energy() >= 62,
          "ACTIVE idle should begin only a slow bounded energy drift");

  updateEveryThreeSeconds(state, 123'000, 300'000, NewoAutonomyEngagement::IDLE);
  require(state.energy() < 62 && state.energy() >= 50,
          "RELAXED idle should deepen fatigue without jumping to drowsy floor");

  updateEveryThreeSeconds(state, 303'000, 1'200'000, NewoAutonomyEngagement::IDLE);
  require(state.energy() == 35, "long DROWSY idle must settle at the bounded energy floor");
  require(state.fatigue() == 65, "fatigue must track the bounded idle floor");
}

void testInteractionRecoveryAndBounds() {
  NewoAutonomyState state;
  state.reset(0);
  updateEveryThreeSeconds(state, 3'000, 1'200'000, NewoAutonomyEngagement::IDLE);
  require(state.energy() == 35, "recovery test requires drowsy floor");
  require(state.stage(1'200'000) == NewoInactivityStage::DROWSY,
          "long idle should be drowsy before interaction");

  state.noteInteraction(1'200'000, 0, 80, 80);
  require(state.stage(1'200'000) == NewoInactivityStage::ACTIVE,
          "interaction must immediately reset the inactivity stage");
  require(state.curiosity() == 100 && state.social() == 100,
          "interaction gains must saturate at 100");

  updateEveryThreeSeconds(state, 1'203'000, 1'218'000, NewoAutonomyEngagement::SPEAKING);
  require(state.energy() == 36, "sustained engagement should recover energy slowly, not jump");
  require(state.lastInteractionMs() == 1'218'000,
          "ongoing engagement must keep inactivity fresh");

  for (int index = 0; index < 20; ++index) state.noteInteraction(1'218'000, 20, 20, 20);
  require(state.energy() == 100 && state.curiosity() == 100 && state.social() == 100,
          "all character-state gains must remain bounded at 100");
}

void testStressAndCuriosityDecay() {
  NewoAutonomyState state;
  state.reset(0);
  state.noteError();
  require(state.stress() == 17, "error should raise stress by the approved bounded amount");
  state.update(3'000, NewoAutonomyEngagement::IDLE);
  require(state.stress() == 16, "stress should decay gradually after an error");

  state.noteInteraction(3'000, 0, 10, 0);
  require(state.curiosity() > NewoAutonomyState::kCuriosityBaseline,
          "interaction should be able to raise curiosity above baseline");
  const uint8_t before = state.curiosity();
  state.update(6'000, NewoAutonomyEngagement::IDLE);
  require(state.curiosity() == static_cast<uint8_t>(before - 1),
          "curiosity should relax toward baseline one step at a time");
}
}  // namespace

int main() {
  testResetAndStages();
  testIdleEnergyLifecycleAndFloor();
  testInteractionRecoveryAndBounds();
  testStressAndCuriosityDecay();
  std::cout << "autonomy-state-host-test: PASS\n";
  return 0;
}
