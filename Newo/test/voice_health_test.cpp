#include <assert.h>
#include "../newo_voice_health.h"

int main() {
  // A: healthy
  NewoVoiceHealth h(20, 750000, 1500000, 2000, 10000);
  assert(!h.observe(0, 5, 0, 0, 60000).reset);
  assert(h.state() == VoiceHealthState::HEALTHY);
  // B: a short queue spike naturally recovers
  assert(h.observe(100, 20, 0, 0, 0).degraded);
  assert(h.observe(1000, 5, 0, 0, 0).recovered);
  assert(!h.observe(1500, 5, 0, 0, 0).reset);
  // C: saturated queue for 2 seconds resets once
  assert(h.observe(2000, 24, 1, 1, 0).degraded);
  assert(!h.observe(3999, 24, 2, 2, 0).reset);
  assert(h.observe(4000, 24, 3, 3, 0).reset);
  h.resetComplete(4000);
  // E: faults during cooldown cannot reset-loop
  assert(!h.observe(5000, 24, 4, 4, 2000000).reset);
  assert(!h.observe(13000, 24, 5, 5, 2000000).reset);
  assert(!h.observe(15000, 24, 6, 6, 2000000).reset);
  assert(h.observe(17000, 24, 7, 7, 2000000).reset);
  // D: one fatal latency starts degradation; repeated/sustained fault resets.
  NewoVoiceHealth s(20, 750000, 1500000, 2000, 10000);
  assert(s.observe(0, 5, 0, 0, 1600000).degraded);
  assert(!s.observe(1000, 5, 0, 0, 800000).reset);
  assert(s.observe(2000, 5, 0, 0, 800000).reset);
}
