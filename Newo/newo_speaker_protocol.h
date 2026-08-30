#pragma once

#include <stddef.h>
#include <stdint.h>

inline bool newoValidSpeakerBegin(uint32_t sampleRate, uint8_t channels, uint8_t bitsPerSample,
                                  bool streaming, uint32_t bytes, uint32_t maxBytes,
                                  uint32_t requiredSampleRate, uint32_t hardMaxBytes) {
  if (sampleRate != requiredSampleRate || channels != 1 || bitsPerSample != 16) return false;
  if (streaming) return bytes == 0 && maxBytes > 0 && maxBytes <= hardMaxBytes && !(maxBytes & 1);
  return bytes > 0 && bytes <= hardMaxBytes && !(bytes & 1) && maxBytes == bytes;
}

inline bool newoValidSpeakerChunk(size_t length, uint32_t receivedBytes, uint32_t limitBytes,
                                  bool endReceived) {
  return !endReceived && length > 0 && !(length & 1) && receivedBytes <= limitBytes &&
         length <= limitBytes - receivedBytes;
}

// Host-testable lifecycle accounting mirrored by the bounded ESP Opus queue.
// It deliberately tracks admission (wire callback) separately from decoded PCM.
struct NewoOpusQueueLifecycle {
  uint16_t depth;
  uint16_t ready;
  uint16_t freeSlots;
  uint16_t expectedSequence;
  uint32_t admittedBytes;
  bool sawPartial;
  bool decoderActive;

  void reset() { ready = 0; freeSlots = depth; expectedSequence = 0; admittedBytes = 0; sawPartial = false; }
  bool readyForPlayback() const { return !decoderActive && ready == 0 && freeSlots == depth; }
  bool admit(uint16_t sequence, uint16_t validBytes, uint16_t fullFrameBytes) {
    if (!readyForAdmission() || sequence != expectedSequence || sawPartial || validBytes == 0 ||
        (validBytes & 1) || validBytes > fullFrameBytes) return false;
    --freeSlots; ++ready; ++expectedSequence; admittedBytes += validBytes;
    if (validBytes < fullFrameBytes) sawPartial = true;
    return true;
  }
  bool releaseDecoded() { if (ready == 0 || freeSlots >= depth) return false; --ready; ++freeSlots; return true; }
 private:
  bool readyForAdmission() const { return decoderActive && freeSlots > 0; }
};

enum class NewoSpeakerEndValidation : uint8_t { OK, WRONG_PLAYBACK_ID, INVALID_BYTES };

inline NewoSpeakerEndValidation newoValidateSpeakerEnd(bool playbackIdMatches, bool streaming,
                                                        uint32_t declaredBytes, uint32_t receivedBytes,
                                                        uint32_t expectedBytes, uint32_t maxBytes) {
  if (!playbackIdMatches) return NewoSpeakerEndValidation::WRONG_PLAYBACK_ID;
  if (declaredBytes == 0 || (declaredBytes & 1) || declaredBytes != receivedBytes ||
      declaredBytes > maxBytes || (!streaming && declaredBytes != expectedBytes)) {
    return NewoSpeakerEndValidation::INVALID_BYTES;
  }
  return NewoSpeakerEndValidation::OK;
}
