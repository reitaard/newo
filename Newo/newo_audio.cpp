#include "newo_audio.h"

#include <cmath>
#include <cstring>
#include <esp_system.h>

#include "newo_log.h"

#if __has_include("newo_secrets.h")
#include "newo_secrets.h"
#define NEWO_AUDIO_HAS_LOCAL_SECRETS 1
#else
#define NEWO_AUDIO_HAS_LOCAL_SECRETS 0
#endif

static_assert(NewoConfig::AUDIO_SAMPLES_PER_FRAME == 320,
              "20 ms at 16 kHz must produce 320 PCM samples");
static_assert(NewoConfig::AUDIO_FRAME_BYTES == 640,
              "Audio frame must be 640 bytes of PCM16");

NewoAudio::NewoAudio(NewoWiFi& wifi) : wifi_(wifi) {}

void NewoAudio::begin() {
#if !NEWO_AUDIO_HAS_LOCAL_SECRETS
  NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "AUDIO_DISABLED", "secrets_missing");
  return;
#else
  if (strlen(NewoSecrets::DEVICE_ID) == 0 || strlen(NewoSecrets::DEVICE_SECRET) < 24 ||
      strlen(NewoSecrets::CLOUD_CA_CERT) < 100) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "AUDIO_DISABLED", "cloud_auth_missing");
    return;
  }

  queue_ = xQueueCreate(NewoConfig::AUDIO_QUEUE_DEPTH, sizeof(AudioFrame));
  if (!queue_) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "AUDIO_DISABLED", "queue_allocation_failed");
    return;
  }

  i2s_.setPins(NewoConfig::AUDIO_I2S_BCLK_PIN, NewoConfig::AUDIO_I2S_WS_PIN, -1,
               NewoConfig::AUDIO_I2S_SD_PIN);
  if (!i2s_.begin(I2S_MODE_STD, NewoConfig::AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                  I2S_SLOT_MODE_STEREO)) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "AUDIO_DISABLED", "i2s_begin_failed");
    return;
  }

  voiceWebSocket_.onEvent(
      [this](WStype_t type, uint8_t* payload, size_t length) { handleVoiceEvent(type, payload, length); });
  voiceWebSocket_.setReconnectInterval(NewoConfig::VOICE_WS_RECONNECT_INTERVAL_MS);
  configured_ = true;
  if (xTaskCreatePinnedToCore(audioTaskEntry, "newo-audio", 4096, this, 2, &task_, 0) != pdPASS) {
    configured_ = false;
    i2s_.end();
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "AUDIO_DISABLED", "task_allocation_failed");
    return;
  }
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "AUDIO_CONFIGURED", "pcm16_16khz_20ms");
#endif
}

void NewoAudio::startVoiceConnection() {
#if NEWO_AUDIO_HAS_LOCAL_SECRETS
  if (!configured_ || started_ || !wifi_.connected()) return;

  String headers;
  headers.reserve(strlen(NewoSecrets::DEVICE_ID) + strlen(NewoSecrets::DEVICE_SECRET) + 64);
  headers += F("X-Newo-Device-Id: ");
  headers += NewoSecrets::DEVICE_ID;
  headers += F("\r\nAuthorization: Bearer ");
  headers += NewoSecrets::DEVICE_SECRET;
  // WebSockets copies headers. Never log this string: it contains the bearer secret.
  voiceWebSocket_.setExtraHeaders(headers.c_str());
  voiceWebSocket_.beginSslWithCA(NewoConfig::CLOUD_HOST, NewoConfig::CLOUD_PORT,
                                 NewoConfig::VOICE_PATH, NewoSecrets::CLOUD_CA_CERT, "");
  started_ = true;
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_CONNECTING");
#endif
}

void NewoAudio::loop() {
  if (!configured_) return;

  if (!wifi_.connected()) {
    if (started_) voiceWebSocket_.disconnect();
    started_ = false;
    voiceConnected_ = false;
    discardQueuedFrames();
    return;
  }

  startVoiceConnection();
  voiceWebSocket_.loop();
  if (!voiceConnected_) return;

  AudioFrame frame;
  if (xQueueReceive(queue_, &frame, 0) != pdTRUE) return;
  if (!voiceWebSocket_.sendBIN(reinterpret_cast<uint8_t*>(frame.samples), sizeof(frame.samples))) {
    ++droppedFrames_;
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "VOICE_FRAME_DROPPED", "send_failed");
  } else {
    ++transmittedFrames_;
  }
}

void NewoAudio::audioTaskEntry(void* context) {
  static_cast<NewoAudio*>(context)->audioTask();
}

void NewoAudio::audioTask() {
  constexpr size_t kStereoWordsPerFrame = NewoConfig::AUDIO_SAMPLES_PER_FRAME * 2;
  while (true) {
    if (!voiceConnected_) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    const size_t bytes = i2s_.readBytes(reinterpret_cast<char*>(inputWords_),
                                        kStereoWordsPerFrame * sizeof(inputWords_[0]));
    if (bytes != kStereoWordsPerFrame * sizeof(inputWords_[0])) {
      ++droppedFrames_;
      continue;
    }

    AudioFrame frame = {};
    for (size_t sample = 0; sample < NewoConfig::AUDIO_SAMPLES_PER_FRAME; ++sample) {
      const size_t channelOffset = sample * 2 + (NewoConfig::AUDIO_I2S_MIC_IS_LEFT ? 0 : 1);
      const int32_t slot32 = inputWords_[channelOffset];
      // INMP441 drives a signed 24-bit sample in bits 31..8 of each 32-bit I2S slot.
      // First recover the signed 24-bit value, then explicitly reduce it to PCM16.
      const int32_t sample24 = slot32 >> 8;
      const int32_t sample16 = sample24 >> 8;
      frame.samples[sample] = static_cast<int16_t>(constrain(sample16, -32768, 32767));
    }

    ++capturedFrames_;
    logLevel(frame);
    if (xQueueSend(queue_, &frame, 0) != pdTRUE) {
      ++droppedFrames_;
      ++queueOverruns_;
    }
  }
}

void NewoAudio::handleVoiceEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      voiceConnected_ = true;
      ++reconnectCount_;
      discardQueuedFrames();
      NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_CONNECTED");
      break;
    case WStype_DISCONNECTED:
      voiceConnected_ = false;
      started_ = false;
      discardQueuedFrames();
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "VOICE_DISCONNECTED");
      break;
    case WStype_TEXT:
      // VPS sends only transcript events here. Keep text on USB Serial, never in the RAM log.
      if (length > 0) Serial.printf("[voice] %.*s\n", static_cast<int>(length), payload);
      break;
    case WStype_ERROR:
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "VOICE_WS_ERROR");
      break;
    default:
      break;
  }
}

void NewoAudio::discardQueuedFrames() {
  if (!queue_) return;
  const UBaseType_t stale = uxQueueMessagesWaiting(queue_);
  if (stale) {
    droppedFrames_ += stale;
    xQueueReset(queue_);
  }
}

void NewoAudio::logLevel(const AudioFrame& frame) {
  const uint32_t now = millis();
  if (now - lastLevelLogMs_ < NewoConfig::AUDIO_LEVEL_LOG_INTERVAL_MS) return;
  lastLevelLogMs_ = now;

  int32_t peak = 0;
  uint64_t sumSquares = 0;
  for (int16_t sample : frame.samples) {
    const int32_t value = sample;
    const int32_t absolute = value < 0 ? -value : value;
    if (absolute > peak) peak = absolute;
    sumSquares += static_cast<uint64_t>(value) * static_cast<uint64_t>(value);
  }
  const uint32_t rms = static_cast<uint32_t>(sqrt(sumSquares / NewoConfig::AUDIO_SAMPLES_PER_FRAME));
  char detail[96];
  snprintf(detail, sizeof(detail), "peak=%ld rms=%lu cap=%lu tx=%lu drop=%lu over=%lu recon=%lu",
           static_cast<long>(peak), static_cast<unsigned long>(rms),
           static_cast<unsigned long>(capturedFrames_), static_cast<unsigned long>(transmittedFrames_),
           static_cast<unsigned long>(droppedFrames_), static_cast<unsigned long>(queueOverruns_),
           static_cast<unsigned long>(reconnectCount_));
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "AUDIO_LEVEL", detail);
}
