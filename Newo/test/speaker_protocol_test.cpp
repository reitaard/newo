#include <assert.h>
#include <string.h>

#include "../newo_speaker_protocol.h"

int main() {
  constexpr uint32_t rate = 24000;
  constexpr uint32_t limit = 2880000;

  assert(newoValidSpeakerBegin(rate, 1, 16, true, 0, limit, rate, limit));
  assert(newoValidSpeakerBegin(rate, 1, 16, false, 4096, 4096, rate, limit));
  assert(!newoValidSpeakerBegin(16000, 1, 16, true, 0, limit, rate, limit));
  assert(!newoValidSpeakerBegin(rate, 1, 16, true, 1, limit, rate, limit));
  assert(!newoValidSpeakerBegin(rate, 1, 16, true, 0, limit + 2, rate, limit));

  assert(newoValidSpeakerChunk(2048, limit - 2048, limit, false));
  assert(!newoValidSpeakerChunk(2049, 0, limit, false));
  assert(!newoValidSpeakerChunk(2048, limit - 1024, limit, false));
  assert(!newoValidSpeakerChunk(2048, 0, limit, true));

  assert(newoValidateSpeakerEnd(true, true, 4096, 4096, 0, limit) == NewoSpeakerEndValidation::OK);
  assert(newoValidateSpeakerEnd(true, false, 4096, 4096, 4096, 4096) == NewoSpeakerEndValidation::OK);
  assert(newoValidateSpeakerEnd(false, true, 4096, 4096, 0, limit) == NewoSpeakerEndValidation::WRONG_PLAYBACK_ID);
  assert(newoValidateSpeakerEnd(true, true, 4096, 2048, 0, limit) == NewoSpeakerEndValidation::INVALID_BYTES);
  assert(newoValidateSpeakerEnd(true, false, 2048, 2048, 4096, 4096) == NewoSpeakerEndValidation::INVALID_BYTES);
  assert(newoValidateSpeakerEnd(true, true, limit + 2, limit + 2, 0, limit) == NewoSpeakerEndValidation::INVALID_BYTES);
}
