#include "newo_speaker_output.h"

#include <cstring>

bool NewoSpeakerOutput::begin(i2s_mode_t mode, uint32_t rate, i2s_data_bit_width_t bits,
                              i2s_slot_mode_t channels, int8_t slotMask,
                              i2s_role_t role) {
  usbActive_ = false;
  usbHealthy_ = true;
  usbStageSamples_ = 0;

  // Keep the original I2S channel alive even for USB playback. NewoSpeaker's
  // proven completion logic counts I2S TX EOF events; when USB is selected we
  // feed this channel silence at the same 24 kHz cadence while the real PCM is
  // sent to the D07. Nothing about the network/Opus buffering path changes.
  if (!physical_.begin(mode, rate, bits, channels, slotMask, role)) return false;

  if (rate == 24'000 && bits == I2S_DATA_BIT_WIDTH_16BIT &&
      channels == I2S_SLOT_MODE_STEREO && newoUsbAudio.speakerReady()) {
    usbActive_ = newoUsbAudio.beginSpeakerPlayback();
    if (usbActive_) {
      Serial.println("[speaker-out] OUTPUT=usb-d07 source=24000Hz-mono sink=48000Hz-stereo volume=unity-at-100%");
    } else {
      Serial.println("[speaker-out] D07 start failed; OUTPUT=i2s-fallback");
    }
  }

  if (!usbActive_) Serial.println("[speaker-out] OUTPUT=i2s");
  return true;
}

bool NewoSpeakerOutput::flushUsbStage(bool finalBatch) {
  if (!usbActive_ || usbStageSamples_ == 0) return true;
  if (!finalBatch && usbStageSamples_ < NewoUsbAudio::kMono24SamplesPerBatch) return true;

  const size_t samples = usbStageSamples_;
  if (!newoUsbAudio.writeSpeakerMono24(usbStage_, samples, 150)) {
    usbHealthy_ = false;
    return false;
  }
  usbStageSamples_ = 0;
  return true;
}

size_t NewoSpeakerOutput::write(const uint8_t* buffer, size_t size) {
  if (buffer == nullptr || size == 0 || (size & 3U) != 0) return 0;

  if (!usbActive_) return physical_.write(buffer, size);
  if (!usbHealthy_ || !newoUsbAudio.speakerPlaying()) return 0;

  // NewoSpeaker has already applied mute/volume and expanded mono source to
  // stereo 24 kHz. Both channels are identical, so collapse each stereo frame
  // back to one sample and stage exact 8 ms USB batches. D07 conversion then
  // repeats each sample into two 48 kHz stereo frames: no gain reduction and no
  // silence gaps between ordinary NewoSpeaker::write() calls.
  const int16_t* stereo = reinterpret_cast<const int16_t*>(buffer);
  const size_t frames = size / (2 * sizeof(int16_t));
  for (size_t frame = 0; frame < frames; ++frame) {
    usbStage_[usbStageSamples_++] = stereo[frame * 2];
    if (usbStageSamples_ == NewoUsbAudio::kMono24SamplesPerBatch && !flushUsbStage(false)) {
      return 0;
    }
  }

  // Preserve NewoSpeaker's existing I2S DMA completion timing without making
  // the MAX98357A audible while the external USB speaker is selected.
  size_t remaining = size;
  while (remaining > 0) {
    const size_t chunk = remaining > sizeof(silentI2s_) ? sizeof(silentI2s_) : remaining;
    if (physical_.write(silentI2s_, chunk) != chunk) {
      usbHealthy_ = false;
      return 0;
    }
    remaining -= chunk;
  }

  if (newoUsbAudio.transferErrors() != 0 || newoUsbAudio.packetErrors() != 0) {
    usbHealthy_ = false;
    return 0;
  }
  return size;
}

bool NewoSpeakerOutput::end() {
  bool ok = true;
  if (usbActive_) {
    if (usbStageSamples_ > 0 && !flushUsbStage(true)) ok = false;
    uint32_t usbDrainMs = 0;
    if (!newoUsbAudio.endSpeakerPlayback(&usbDrainMs)) ok = false;
    Serial.printf("[speaker-out] USB_DRAIN — healthy=%u drain_ms=%lu\n",
                  ok ? 1U : 0U, static_cast<unsigned long>(usbDrainMs));
  }
  usbActive_ = false;
  usbHealthy_ = ok;
  usbStageSamples_ = 0;
  return physical_.end() && ok;
}
