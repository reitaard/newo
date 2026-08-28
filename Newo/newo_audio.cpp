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
static_assert(NewoConfig::AUDIO_WS_BUNDLE_FRAMES > 0 &&
                  NewoConfig::AUDIO_WS_BUNDLE_FRAMES <= NewoConfig::AUDIO_QUEUE_DEPTH,
              "Audio bundle must fit in the bounded queue");
static_assert(NewoConfig::AUDIO_WS_BUNDLE_FRAMES <= NewoConfig::AUDIO_SEND_DRAIN_FRAME_LIMIT,
              "Sender budget must fit one complete audio bundle");

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
  if (xTaskCreatePinnedToCore(audioTaskEntry, "newo-audio", 4096, this, 2, &task_, 0) != pdPASS ||
      xTaskCreatePinnedToCore(voiceTaskEntry, "newo-voice", 6144, this, 2, &voiceTask_, 1) != pdPASS) {
    configured_ = false;
    if (task_) vTaskDelete(task_);
    i2s_.end();
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "AUDIO_DISABLED", "task_allocation_failed");
    return;
  }
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "AUDIO_CONFIGURED", "pcm16_16khz_20ms_voice_task");
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
  // `/device`, Wi-Fi, and the Arduino loop never service the voice socket.
  // resetVoiceStream() only sets a request consumed by the voice task.
}

void NewoAudio::voiceTaskEntry(void* context) {
  static_cast<NewoAudio*>(context)->voiceTask();
}

void NewoAudio::voiceTask() {
  while (true) {
    if (resetRequested_) {
      const char* reason = resetReason_;
      resetRequested_ = false;
      performVoiceReset(reason);
    }
    if (!wifi_.connected()) {
      if (started_) voiceWebSocket_.disconnect();
      started_ = false;
      voiceConnected_ = false;
      discardQueuedFrames();
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    startVoiceConnection();
    voiceWebSocket_.loop();
    if (!voiceConnected_) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    AudioFrame frame;
    if (uxQueueMessagesWaiting(queue_) >= NewoConfig::AUDIO_WS_BUNDLE_FRAMES) {
      for (size_t index = 0; index < NewoConfig::AUDIO_WS_BUNDLE_FRAMES; ++index) {
        if (xQueueReceive(queue_, &frame, 0) != pdTRUE) break;
        memcpy(bundle_ + (index * sizeof(frame.samples)), frame.samples, sizeof(frame.samples));
      }
      const uint32_t sendStartedUs = micros();
      const bool sent = voiceWebSocket_.sendBIN(bundle_, sizeof(bundle_));
      const uint32_t sendDurationUs = micros() - sendStartedUs;
      if (sendDurationUs > maxSendDurationUsSinceLevel_) maxSendDurationUsSinceLevel_ = sendDurationUs;
      evaluateVoiceHealth(sendDurationUs);
      if (resetRequested_ || !voiceConnected_) continue;
      if (!sent) {
        droppedFrames_ += NewoConfig::AUDIO_WS_BUNDLE_FRAMES;
        NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "VOICE_FRAME_DROPPED", "send_failed");
      } else {
        transmittedFrames_ += NewoConfig::AUDIO_WS_BUNDLE_FRAMES;
        ++bundleSends_;
      }
    } else {
      evaluateVoiceHealth();
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

const char* NewoAudio::healthReasonName(VoiceHealthReason reason) {
  switch (reason) {
    case VoiceHealthReason::QUEUE: return "queue";
    case VoiceHealthReason::SEND_LATENCY: return "send_latency";
    case VoiceHealthReason::DROPS: return "drops";
    default: return "unknown";
  }
}

void NewoAudio::evaluateVoiceHealth(uint32_t sendDurationUs) {
  if (!queue_ || !voiceConnected_) return;
  const uint8_t queueDepth = static_cast<uint8_t>(uxQueueMessagesWaiting(queue_));
  const uint32_t dropDelta = droppedFrames_ - lastHealthDropped_;
  const uint32_t overrunDelta = queueOverruns_ - lastHealthOverruns_;
  lastHealthDropped_ = droppedFrames_;
  lastHealthOverruns_ = queueOverruns_;
  const VoiceHealthDecision decision = voiceHealth_.observe(millis(), queueDepth, droppedFrames_, queueOverruns_, sendDurationUs);
  if (decision.degraded) {
    char detail[96];
    snprintf(detail, sizeof(detail), "reason=%s q=%u age=%ums dd=%lu od=%lu su=%lu", healthReasonName(decision.reason),
             static_cast<unsigned>(queueDepth), static_cast<unsigned>(queueDepth * NewoConfig::AUDIO_FRAME_DURATION_MS),
             static_cast<unsigned long>(dropDelta), static_cast<unsigned long>(overrunDelta),
             static_cast<unsigned long>(sendDurationUs));
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "VOICE_HEALTH_DEGRADED", detail);
  }
  if (decision.recovered) NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_HEALTH_RECOVERED");
  if (decision.reset) {
    char reason[32];
    snprintf(reason, sizeof(reason), "auto_%s", healthReasonName(decision.reason));
    ++automaticVoiceResets_;
    resetVoiceStream(reason);
  }
}

void NewoAudio::resetVoiceStream(const char* reason) {
  if (!configured_) return;
  resetReason_ = reason ? reason : "manual";
  resetRequested_ = true;
}

void NewoAudio::performVoiceReset(const char* reason) {
  if (!configured_) return;
  char detail[64];
  snprintf(detail, sizeof(detail), "reason=%s ar=%lu", reason ? reason : "manual", static_cast<unsigned long>(automaticVoiceResets_));
  NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "VOICE_RESET_START", detail);
  voiceConnected_ = false;
  started_ = false;
  voiceWebSocket_.disconnect();
  const uint32_t flushed = discardQueuedFrames();
  memset(bundle_, 0, sizeof(bundle_));
  voiceHealth_.resetComplete(millis());
  lastHealthDropped_ = droppedFrames_;
  lastHealthOverruns_ = queueOverruns_;
  resetReconnectPending_ = true;
  snprintf(detail, sizeof(detail), "frames=%lu", static_cast<unsigned long>(flushed));
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_RESET_QUEUE_FLUSHED", detail);
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_RESET_RECONNECTING");
  startVoiceConnection();
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
      // Apply fixed gain while its 24-bit precision is still available. int64_t
      // keeps the multiplication and subsequent reduction safely in range.
      const int32_t sample24 = slot32 >> 8;
      const int64_t gainedSample24 = static_cast<int64_t>(sample24) * NewoConfig::AUDIO_MIC_GAIN;
      const int64_t pcm16 = gainedSample24 >> 8;
      if (pcm16 > INT16_MAX) {
        frame.samples[sample] = INT16_MAX;
        ++clippedSamplesSinceLevel_;
      } else if (pcm16 < INT16_MIN) {
        frame.samples[sample] = INT16_MIN;
        ++clippedSamplesSinceLevel_;
      } else {
        frame.samples[sample] = static_cast<int16_t>(pcm16);
      }
      ++pcmSamplesSinceLevel_;
    }

    ++capturedFrames_;
    logLevel(frame);
    if (xQueueSend(queue_, &frame, 0) != pdTRUE) {
      ++droppedFrames_;
      ++queueOverruns_;
    } else {
      const UBaseType_t queueDepth = uxQueueMessagesWaiting(queue_);
      if (queueDepth > queueHighWaterSinceLevel_) queueHighWaterSinceLevel_ = queueDepth;
    }
  }
}

void NewoAudio::handleVoiceEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      voiceConnected_ = true;
      ++reconnectCount_;
      discardQueuedFrames();
      if (resetReconnectPending_) {
        resetReconnectPending_ = false;
        NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_RESET_CONNECTED");
      }
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

uint32_t NewoAudio::discardQueuedFrames() {
  if (!queue_) return 0;
  const UBaseType_t stale = uxQueueMessagesWaiting(queue_);
  if (stale) {
    droppedFrames_ += stale;
    xQueueReset(queue_);
  }
  return static_cast<uint32_t>(stale);
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
  const uint32_t clipped = clippedSamplesSinceLevel_;
  const uint32_t observedSamples = pcmSamplesSinceLevel_;
  const uint32_t maxSendDurationUs = maxSendDurationUsSinceLevel_;
  const UBaseType_t queueHighWater = queueHighWaterSinceLevel_;
  // Tenths of a percent avoids float formatting and makes low clipping visible.
  const uint32_t clipPercentTenths = observedSamples == 0
      ? 0
      : (clipped * 1'000U + observedSamples / 2U) / observedSamples;
  clippedSamplesSinceLevel_ = 0;
  pcmSamplesSinceLevel_ = 0;
  maxSendDurationUsSinceLevel_ = 0;
  queueHighWaterSinceLevel_ = 0;

  char detail[96];
  snprintf(detail, sizeof(detail), "p=%ld r=%lu c=%lu t=%lu d=%lu o=%lu b=%lu su=%lu q=%u ar=%lu cl=%lu/%lu.%lu%%",
           static_cast<long>(peak), static_cast<unsigned long>(rms),
           static_cast<unsigned long>(capturedFrames_), static_cast<unsigned long>(transmittedFrames_),
           static_cast<unsigned long>(droppedFrames_), static_cast<unsigned long>(queueOverruns_),
           static_cast<unsigned long>(bundleSends_), static_cast<unsigned long>(maxSendDurationUs),
           static_cast<unsigned>(queueHighWater), static_cast<unsigned long>(automaticVoiceResets_),
           static_cast<unsigned long>(clipped),
           static_cast<unsigned long>(clipPercentTenths / 10),
           static_cast<unsigned long>(clipPercentTenths % 10));
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "AUDIO_LEVEL", detail);
}
