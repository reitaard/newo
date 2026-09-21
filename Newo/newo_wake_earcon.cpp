#include "newo_wake_earcon.h"

#include <ESP_I2S.h>
#include <Preferences.h>
#include <cmath>
#include <cstring>
#include <driver/i2s_common.h>

#include "newo_config.h"
#include "newo_log.h"
#include "newo_storage.h"
#include "newo_usb_audio.h"

// NewoStorage is the single persisted speaker-volume/mute source owned by the
// sketch. The earcon has its own tiny NVS preference because it is an input UX
// setting, not part of the network speaker transport.
extern NewoStorage newoStorage;

namespace {
constexpr char kNamespace[] = "newo-earcon";
constexpr char kModeKey[] = "mode";
constexpr uint8_t kModeOff = 0;
constexpr uint8_t kModeRotate = 1;
constexpr uint8_t kModeChime = 2;
constexpr uint8_t kModeSweep = 3;
constexpr uint8_t kModeTick = 4;
constexpr uint32_t kSampleRate = NewoConfig::SPEAKER_SAMPLE_RATE;
// No silent post-cue guard: once the physical TX tail is drained the capture
// gate opens immediately. The tone itself has a fade-out to avoid a hard edge.
constexpr uint32_t kAcousticGuardMs = 0;
constexpr float kTwoPi = 6.2831853071795864769f;
constexpr int32_t kBaseAmplitude = 6000;
constexpr size_t kI2sChunkFrames = 128;

Preferences preferences;
bool preferencesReady = false;
volatile uint8_t selectedMode = kModeRotate;
uint32_t rotationSequence = 0;
volatile uint32_t sentEvents = 0;

bool IRAM_ATTR onEarconSent(i2s_chan_handle_t, i2s_event_data_t*, void* userData) {
  auto* counter = static_cast<volatile uint32_t*>(userData);
  if (counter) ++(*counter);
  return false;
}

bool ensurePreferences() {
  if (preferencesReady) return true;
  if (!preferences.begin(kNamespace, false)) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO,
                 "EARCON_STORAGE_FAILED");
    return false;
  }
  uint8_t mode = preferences.getUChar(kModeKey, kModeRotate);
  if (mode > kModeTick) mode = kModeRotate;
  selectedMode = mode;
  preferencesReady = true;
  return true;
}

const char* modeName(uint8_t mode) {
  switch (mode) {
    case kModeOff: return "off";
    case kModeRotate: return "rotate";
    case kModeChime: return "chime";
    case kModeSweep: return "sweep";
    case kModeTick: return "tick";
    default: return "rotate";
  }
}

bool parseMode(const char* value, uint8_t& mode) {
  if (!value) return false;
  if (strcmp(value, "off") == 0 || strcmp(value, "none") == 0) mode = kModeOff;
  else if (strcmp(value, "rotate") == 0 || strcmp(value, "auto") == 0 || strcmp(value, "on") == 0) mode = kModeRotate;
  else if (strcmp(value, "chime") == 0 || strcmp(value, "1") == 0) mode = kModeChime;
  else if (strcmp(value, "sweep") == 0 || strcmp(value, "2") == 0) mode = kModeSweep;
  else if (strcmp(value, "tick") == 0 || strcmp(value, "3") == 0) mode = kModeTick;
  else return false;
  return true;
}

float fadeEnvelope(uint32_t localFrame, uint32_t segmentFrames) {
  if (segmentFrames <= 1) return 0.0f;
  const uint32_t fadeFrames = kSampleRate * 8 / 1000;  // soft 8 ms edge.
  const float attack = localFrame >= fadeFrames ? 1.0f :
      static_cast<float>(localFrame) / static_cast<float>(fadeFrames);
  const uint32_t remaining = segmentFrames - 1 - localFrame;
  const float release = remaining >= fadeFrames ? 1.0f :
      static_cast<float>(remaining) / static_cast<float>(fadeFrames);
  return attack < release ? attack : release;
}

struct EarconShape {
  uint8_t mode;
  uint32_t frames;
};

EarconShape resolveShape(uint8_t configured) {
  if (configured != kModeRotate) {
    const uint32_t frames = configured == kModeChime ? kSampleRate * 120 / 1000 :
                            configured == kModeSweep ? kSampleRate * 95 / 1000 :
                            kSampleRate * 80 / 1000;
    return {configured, frames};
  }
  const uint8_t variants[] = {kModeChime, kModeSweep, kModeTick};
  const uint8_t resolved = variants[rotationSequence++ % 3];
  const uint32_t frames = resolved == kModeChime ? kSampleRate * 120 / 1000 :
                          resolved == kModeSweep ? kSampleRate * 95 / 1000 :
                          kSampleRate * 80 / 1000;
  return {resolved, frames};
}

int16_t sampleFor(const EarconShape& shape, uint32_t frame, int32_t amplitude) {
  if (shape.mode == kModeChime) {
    const uint32_t firstFrames = kSampleRate * 50 / 1000;
    const uint32_t gapFrames = kSampleRate * 10 / 1000;
    if (frame < firstFrames) {
      const float env = fadeEnvelope(frame, firstFrames);
      const float phase = kTwoPi * 1900.0f * static_cast<float>(frame) / static_cast<float>(kSampleRate);
      return static_cast<int16_t>(sinf(phase) * env * amplitude);
    }
    if (frame < firstFrames + gapFrames) return 0;
    const uint32_t local = frame - firstFrames - gapFrames;
    const uint32_t secondFrames = shape.frames - firstFrames - gapFrames;
    const float env = fadeEnvelope(local, secondFrames);
    const float phase = kTwoPi * 2700.0f * static_cast<float>(local) / static_cast<float>(kSampleRate);
    return static_cast<int16_t>(sinf(phase) * env * amplitude);
  }

  if (shape.mode == kModeSweep) {
    const float seconds = static_cast<float>(frame) / static_cast<float>(kSampleRate);
    const float duration = static_cast<float>(shape.frames) / static_cast<float>(kSampleRate);
    const float f0 = 1500.0f;
    const float f1 = 3200.0f;
    const float slope = (f1 - f0) / duration;
    const float phase = kTwoPi * (f0 * seconds + 0.5f * slope * seconds * seconds);
    return static_cast<int16_t>(sinf(phase) * fadeEnvelope(frame, shape.frames) * amplitude);
  }

  const uint32_t firstFrames = kSampleRate * 25 / 1000;
  const uint32_t gapFrames = kSampleRate * 20 / 1000;
  const uint32_t secondFrames = shape.frames - firstFrames - gapFrames;
  if (frame < firstFrames) {
    const float phase = kTwoPi * 2250.0f * static_cast<float>(frame) / static_cast<float>(kSampleRate);
    return static_cast<int16_t>(sinf(phase) * fadeEnvelope(frame, firstFrames) * amplitude);
  }
  if (frame < firstFrames + gapFrames) return 0;
  const uint32_t local = frame - firstFrames - gapFrames;
  const float phase = kTwoPi * 2950.0f * static_cast<float>(local) / static_cast<float>(kSampleRate);
  return static_cast<int16_t>(sinf(phase) * fadeEnvelope(local, secondFrames) * amplitude);
}

void logStart(const EarconShape& shape, uint8_t configured, uint8_t volume, const char* output) {
  char detail[112];
  snprintf(detail, sizeof(detail), "mode=%s configured=%s volume=%u output=%s",
           modeName(shape.mode), modeName(configured), static_cast<unsigned>(volume), output);
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
               "EARCON_START", detail);
}

void logEnd(const EarconShape& shape, uint32_t startedMs, uint32_t drainMs,
            const char* output) {
  char detail[128];
  snprintf(detail, sizeof(detail),
           "mode=%s output=%s total_ms=%lu drain_ms=%lu guard_ms=%lu",
           modeName(shape.mode), output,
           static_cast<unsigned long>(millis() - startedMs),
           static_cast<unsigned long>(drainMs),
           static_cast<unsigned long>(kAcousticGuardMs));
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
               "EARCON_END", detail);
}

bool playUsbEarcon(const EarconShape& shape, int32_t amplitude, uint32_t* drainMs) {
  if (!newoUsbAudio.beginSpeakerPlayback()) return false;

  int16_t mono[NewoUsbAudio::kMono24SamplesPerBatch];
  uint32_t frame = 0;
  bool writeOk = true;
  while (frame < shape.frames) {
    const size_t count = min(static_cast<size_t>(shape.frames - frame),
                             NewoUsbAudio::kMono24SamplesPerBatch);
    for (size_t i = 0; i < count; ++i) {
      mono[i] = sampleFor(shape, frame + static_cast<uint32_t>(i), amplitude);
    }
    if (!newoUsbAudio.writeSpeakerMono24(mono, count, 150)) {
      writeOk = false;
      break;
    }
    frame += static_cast<uint32_t>(count);
  }

  uint32_t localDrainMs = 0;
  const bool endOk = newoUsbAudio.endSpeakerPlayback(&localDrainMs);
  if (drainMs) *drainMs = localDrainMs;
  return writeOk && endOk;
}

bool playI2sEarcon(const EarconShape& shape, int32_t amplitude, uint32_t* drainMs) {
  I2SClass output(I2S_NUM_1);
  output.setPins(NewoConfig::SPEAKER_I2S_BCLK_PIN, NewoConfig::SPEAKER_I2S_WS_PIN,
                 NewoConfig::SPEAKER_I2S_DOUT_PIN);
  if (!output.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO,
                 "EARCON_FAILED", "reason=i2s_begin");
    return false;
  }

  i2s_chan_handle_t tx = output.txChan();
  i2s_event_callbacks_t callbacks = {};
  callbacks.on_sent = &onEarconSent;
  sentEvents = 0;
  if (!tx || i2s_channel_disable(tx) != ESP_OK ||
      i2s_channel_register_event_callback(tx, &callbacks,
                                          const_cast<uint32_t*>(&sentEvents)) != ESP_OK ||
      i2s_channel_enable(tx) != ESP_OK) {
    output.end();
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO,
                 "EARCON_FAILED", "reason=i2s_callback");
    return false;
  }

  int16_t stereo[kI2sChunkFrames * 2];
  uint32_t frame = 0;
  bool writeOk = true;
  while (frame < shape.frames) {
    const size_t count = min(static_cast<size_t>(shape.frames - frame), kI2sChunkFrames);
    for (size_t i = 0; i < count; ++i) {
      const int16_t sample = sampleFor(shape, frame + static_cast<uint32_t>(i), amplitude);
      stereo[i * 2] = sample;
      stereo[i * 2 + 1] = sample;
    }
    const size_t bytes = count * 2 * sizeof(int16_t);
    if (output.write(reinterpret_cast<const uint8_t*>(stereo), bytes) != bytes) {
      writeOk = false;
      break;
    }
    frame += static_cast<uint32_t>(count);
  }

  const uint32_t drainStartedMs = millis();
  const uint32_t drainStartEvents = sentEvents;
  if (writeOk) {
    while (static_cast<uint32_t>(sentEvents - drainStartEvents) <
           NewoConfig::SPEAKER_I2S_DRAIN_DMA_EVENTS) {
      if (millis() - drainStartedMs >= NewoConfig::SPEAKER_I2S_DRAIN_TIMEOUT_MS) {
        writeOk = false;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  const uint32_t localDrainMs = millis() - drainStartedMs;
  output.end();
  if (drainMs) *drainMs = localDrainMs;

  if (!writeOk) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO,
                 "EARCON_FAILED", "reason=i2s_write_or_drain");
  }
  return writeOk;
}
}  // namespace

const char* newoWakeEarconModeName() {
  ensurePreferences();
  return modeName(selectedMode);
}

bool newoSetWakeEarconMode(const char* value) {
  uint8_t mode = kModeRotate;
  if (!parseMode(value, mode) || !ensurePreferences()) return false;
  if (mode == selectedMode) return true;
  if (preferences.putUChar(kModeKey, mode) != sizeof(mode)) return false;
  selectedMode = mode;
  return true;
}

bool newoPlayWakeEarcon() {
  ensurePreferences();
  const uint8_t configured = selectedMode;
  const uint8_t volume = newoStorage.speakerVolume();
  const bool muted = newoStorage.speakerMuted();
  if (configured == kModeOff || muted || volume == 0) {
    char detail[64];
    snprintf(detail, sizeof(detail), "mode=%s volume=%u muted=%s",
             modeName(configured), static_cast<unsigned>(volume), muted ? "yes" : "no");
    NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
                 "EARCON_SKIPPED", detail);
    return false;
  }

  const EarconShape shape = resolveShape(configured);
  const int32_t amplitude = kBaseAmplitude * static_cast<int32_t>(volume) / 100;
  const bool usbSelected = newoStorage.usbHostEnabled() && newoStorage.usbAudioEnabled();

  if (usbSelected) {
    if (newoUsbAudio.speakerReady()) {
      const uint32_t startedMs = millis();
      uint32_t drainMs = 0;
      logStart(shape, configured, volume, "usb-d07");
      if (playUsbEarcon(shape, amplitude, &drainMs)) {
        if (kAcousticGuardMs) vTaskDelay(pdMS_TO_TICKS(kAcousticGuardMs));
        logEnd(shape, startedMs, drainMs, "usb-d07");
        return true;
      }
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO,
                   "EARCON_USB_FALLBACK", "reason=transport_failed");
    } else {
      NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
                   "EARCON_USB_FALLBACK", "reason=d07_not_ready");
    }
  }

  const uint32_t startedMs = millis();
  uint32_t drainMs = 0;
  logStart(shape, configured, volume, usbSelected ? "i2s-fallback" : "i2s");
  if (!playI2sEarcon(shape, amplitude, &drainMs)) return false;
  if (kAcousticGuardMs) vTaskDelay(pdMS_TO_TICKS(kAcousticGuardMs));
  logEnd(shape, startedMs, drainMs, usbSelected ? "i2s-fallback" : "i2s");
  return true;
}
