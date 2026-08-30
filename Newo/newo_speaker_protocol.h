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
