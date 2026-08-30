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

  // Queue lifecycle resets every playback, including after cancellation/failure.
  NewoOpusQueueLifecycle queue = {16, 0, 16, 0, 0, false, false};
  assert(queue.readyForPlayback());
  queue.decoderActive = true;
  assert(queue.admit(0, 1920, 1920));
  assert(queue.admit(1, 100, 1920));
  assert(queue.ready == 2 && queue.freeSlots == 14 && queue.expectedSequence == 2 && queue.admittedBytes == 2020);
  assert(!queue.admit(2, 1920, 1920));  // partial frame must be final
  queue.reset();
  queue.decoderActive = false;
  assert(queue.readyForPlayback());
  assert(queue.expectedSequence == 0 && queue.admittedBytes == 0 && !queue.sawPartial);
  // Result publication is only valid after the decoder has relinquished ownership.
  queue.decoderActive = true;
  assert(!queue.readyForPlayback());
  queue.decoderActive = false;
  assert(queue.readyForPlayback());
}
