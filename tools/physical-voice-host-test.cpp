#include "../Newo/newo_physical_voice.h"
#include "../arduino/nano-reset-voice-test/nano_reset_policy.h"

#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
  NanoResetPolicy::TriggerOnce external;
  external.arm(NanoResetPolicy::kExternal);
  assert(external.take());
  assert(!external.take());

  NanoResetPolicy::TriggerOnce powerOn;
  powerOn.arm(NanoResetPolicy::kPowerOn);
  assert(!powerOn.take());
  assert(std::strcmp(NanoResetPolicy::causeName(NanoResetPolicy::kWatchdog), "watchdog") == 0);

  NewoPhysicalVoice::TriggerGate gate;
  assert(gate.decide(1, false, false) == NewoPhysicalVoice::TriggerDecision::ACCEPT);
  assert(gate.decide(1, false, false) == NewoPhysicalVoice::TriggerDecision::DUPLICATE);
  assert(gate.decide(2, true, false) == NewoPhysicalVoice::TriggerDecision::VOICE_ACTIVE);
  assert(gate.decide(3, false, true) == NewoPhysicalVoice::TriggerDecision::SPEAKER_BUSY);
  assert(gate.decide(4, false, false) == NewoPhysicalVoice::TriggerDecision::ACCEPT);

  using NewoPhysicalVoice::LedState;
  assert(std::strcmp(NewoPhysicalVoice::ledPayload(LedState::LISTENING), "on") == 0);
  assert(std::strcmp(NewoPhysicalVoice::ledPayload(LedState::THINKING), "blink_fast") == 0);
  assert(std::strcmp(NewoPhysicalVoice::ledPayload(LedState::SPEAKING), "blink_slow") == 0);
  assert(std::strcmp(NewoPhysicalVoice::ledPayload(LedState::ERROR), "error") == 0);
  assert(std::strcmp(NewoPhysicalVoice::ledPayload(LedState::IDLE), "off") == 0);

  std::puts("Physical voice policy tests passed");
}
