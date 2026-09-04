#include "newo_audio.h"

#include <cmath>
#include <cstring>

#include "newo_log.h"

#if __has_include("newo_secrets.h")
#include "newo_secrets.h"
#define NEWO_AUDIO_HAS_LOCAL_SECRETS 1
#else
#define NEWO_AUDIO_HAS_LOCAL_SECRETS 0
#endif

NewoAudio* NewoAudio::instance_ = nullptr;

NewoAudio::NewoAudio(NewoWiFi& wifi, NewoDisplay& display) : wifi_(wifi), display_(display) {}

void NewoAudio::begin() {
  instance_ = this;
  voiceWebSocket_.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
    handleVoiceEvent(type, payload, length);
  });
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_OFF_LOCAL_WAKE_READY");
  if (enabled_) setEnabled(true);
}

bool NewoAudio::configureI2s() {
  if (i2sRunning_) return true;
  i2s_.setPins(NewoConfig::AUDIO_I2S_BCLK_PIN, NewoConfig::AUDIO_I2S_WS_PIN, -1,
               NewoConfig::AUDIO_I2S_SD_PIN);
  // ESP_SR accepts PCM16. The Arduino supplied RX transform converts the
  // INMP441's 32-bit I2S slots once, before either WakeNet or streaming sees it.
  if (!i2s_.begin(I2S_MODE_STD, NewoConfig::AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                  I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_LEFT) ||
      !i2s_.configureRX(NewoConfig::AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                         I2S_SLOT_MODE_STEREO, I2S_RX_TRANSFORM_32_TO_16)) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "VOICE_I2S_FAILED");
    i2s_.end();
    return false;
  }
  i2sRunning_ = true;
  return true;
}

void NewoAudio::releaseI2s() {
  if (!i2sRunning_) return;
  i2s_.end();
  i2sRunning_ = false;
}

bool NewoAudio::startWakeNet() {
  if (!enabled_ || playbackSuppressed_ || wakeNetRunning_) return wakeNetRunning_;
  if (!configureI2s()) return false;
  ESP_SR.onEvent(srEvent);
  // Empty commands keep Newo in SR_MODE_WAKEWORD; it never enters command mode.
  if (!ESP_SR.begin(i2s_, nullptr, 0, SR_CHANNELS_STEREO, SR_MODE_WAKEWORD, "MN")) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "WAKENET_START_FAILED");
    releaseI2s();
    return false;
  }
  wakeNetRunning_ = true;
  state_ = NewoVoiceState::ARMED;
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "WAKENET_ARMED");
  return true;
}

void NewoAudio::stopWakeNet() {
  if (!wakeNetRunning_) return;
  // end waits for ESP_SR feed/detect tasks to stop, so I2S is not concurrently
  // read when the streaming task is created.
  ESP_SR.end();
  wakeNetRunning_ = false;
  releaseI2s();
}

bool NewoAudio::setPlaybackActive(bool active) {
  if (active) {
    if (state_ == NewoVoiceState::STREAMING) return false;
    playbackSuppressed_ = true;
    if (state_ == NewoVoiceState::ARMED) stopWakeNet();
    NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "WAKENET_SPEAKER_SUPPRESSED");
    return true;
  }
  if (!playbackSuppressed_) return true;
  playbackSuppressed_ = false;
  if (enabled_ && state_ != NewoVoiceState::STREAMING) {
    state_ = NewoVoiceState::ARMED;
    if (!startWakeNet()) state_ = NewoVoiceState::OFF;
  } else if (!enabled_) {
    state_ = NewoVoiceState::OFF;
  }
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "WAKENET_SPEAKER_RESTORED");
  return true;
}

bool NewoAudio::setEnabled(bool enabled) {
  enabled_ = enabled;
  if (!enabled) {
    wakePending_ = false;
    if (state_ == NewoVoiceState::STREAMING) {
      stopStreaming_ = true;
      transitionPending_ = true;
      return false;
    }
    stopWakeNet();
    state_ = NewoVoiceState::OFF;
    transitionPending_ = false;
    NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_OFF");
    return true;
  }
  if (state_ == NewoVoiceState::OFF) {
    if (playbackSuppressed_) {
      state_ = NewoVoiceState::ARMED;
      transitionPending_ = false;
      return true;
    }
    const bool armed = startWakeNet();
    transitionPending_ = false;
    return armed;
  }
  return state_ == NewoVoiceState::ARMED;
}

void NewoAudio::srEvent(sr_event_t event, int, int) {
  if (instance_ && event == SR_EVENT_WAKEWORD && instance_->state_ == NewoVoiceState::ARMED) {
    instance_->wakePending_ = true;
  }
}

bool NewoAudio::beginStreaming(bool rearmAfterStream) {
#if !NEWO_AUDIO_HAS_LOCAL_SECRETS
  ++failures_;
  NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "VOICE_STREAM_DISABLED", "secrets_missing");
  return false;
#else
  if (state_ == NewoVoiceState::STREAMING || streamTask_ || playbackSuppressed_) return false;
  if (rearmAfterStream && (!enabled_ || state_ != NewoVoiceState::ARMED)) return false;
  // A future WakeNet event and a manual request share this single stream task.
  // StopWakeNet also releases its I2S ownership before direct capture begins.
  stopWakeNet();
  if (!configureI2s()) { ++failures_; return false; }
  rearmAfterStream_ = rearmAfterStream;
  if (!rearmAfterStream_) enabled_ = false;
  display_.setListeningActive(true);
  state_ = NewoVoiceState::STREAMING;
  transitionPending_ = true;
  streamFinished_ = false;
  stopStreaming_ = false;
  streamEndReason_ = nullptr;
  streamStartedMs_ = millis();
  ++sessionCount_;
  // TLS/WebSocket setup and two PCM frame buffers share this task's stack.
  // Match the proven speaker task allocation rather than using the smaller
  // temporary-stream allocation.
  if (xTaskCreatePinnedToCore(streamTaskEntry, "newo-voice", 8192, this, 2, &streamTask_, 1) != pdPASS) {
    ++failures_;
    streamEndReason_ = "task_failed";
    streamFinished_ = true;
    display_.setListeningActive(false);
    display_.noteSystemError();
    return false;
  }
  // I2S and the one task are now owned by STREAMING. Ack the control request
  // promptly; WebSocket/ASR/TTS completion remains independent.
  transitionPending_ = false;
  return true;
#endif
}

bool NewoAudio::manualToggle() {
  if (state_ == NewoVoiceState::STREAMING) {
    setEnabled(false);
    return true;
  }
  if (playbackSuppressed_) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "VOICE_MANUAL_BUSY", "speaker_playback");
    return false;
  }
  // ARMED is preserved for future WakeNet work, but a manual turn takes the
  // microphone directly and always settles back to OFF.
  return beginStreaming(false);
}

void NewoAudio::streamTaskEntry(void* context) { static_cast<NewoAudio*>(context)->streamTask(); }

void NewoAudio::streamTask() {
#if NEWO_AUDIO_HAS_LOCAL_SECRETS
  if (!configureI2s()) { streamEndReason_ = "i2s_failed"; streamFinished_ = true; vTaskDelete(nullptr); return; }
  String headers;
  headers.reserve(strlen(NewoSecrets::DEVICE_ID) + strlen(NewoSecrets::DEVICE_SECRET) + 64);
  headers += F("X-Newo-Device-Id: "); headers += NewoSecrets::DEVICE_ID;
  headers += F("\r\nAuthorization: Bearer "); headers += NewoSecrets::DEVICE_SECRET;
  voiceWebSocket_.setExtraHeaders(headers.c_str());
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_CONNECTING");
  voiceWebSocket_.beginSslWithCA(NewoConfig::CLOUD_HOST, NewoConfig::CLOUD_PORT,
                                 NewoConfig::VOICE_PATH, NewoSecrets::CLOUD_CA_CERT, "");
  int16_t stereo[NewoConfig::AUDIO_SAMPLES_PER_FRAME * 2];
  int16_t mono[NewoConfig::AUDIO_SAMPLES_PER_FRAME];
  constexpr uint32_t kHealthFrames = 25;  // First 0.5 s at 20 ms/frame.
  uint32_t healthFrames = 0;
  uint32_t healthSamples = 0;
  uint32_t healthNonzero = 0;
  uint32_t healthPeak = 0;
  uint64_t healthSquareSum = 0;
  int16_t healthMin = 32767;
  int16_t healthMax = -32768;
  while (!stopStreaming_) {
    voiceWebSocket_.loop();
    if (voiceConnected_) {
      if (i2s_.readBytes(reinterpret_cast<char*>(stereo), sizeof(stereo)) != sizeof(stereo)) {
        streamEndReason_ = "i2s_read_failed";
        break;
      }
      for (size_t i = 0; i < NewoConfig::AUDIO_SAMPLES_PER_FRAME; ++i) {
        mono[i] = stereo[i * 2 + (NewoConfig::AUDIO_I2S_MIC_IS_LEFT ? 0 : 1)];
      }
      if (healthFrames < kHealthFrames) {
        for (size_t i = 0; i < NewoConfig::AUDIO_SAMPLES_PER_FRAME; ++i) {
          const int32_t sample = mono[i];
          const uint32_t absolute = sample < 0 ? static_cast<uint32_t>(-sample) : static_cast<uint32_t>(sample);
          if (sample != 0) ++healthNonzero;
          if (absolute > healthPeak) healthPeak = absolute;
          if (sample < healthMin) healthMin = static_cast<int16_t>(sample);
          if (sample > healthMax) healthMax = static_cast<int16_t>(sample);
          healthSquareSum += static_cast<uint64_t>(sample * sample);
        }
        ++healthFrames;
        healthSamples += NewoConfig::AUDIO_SAMPLES_PER_FRAME;
        if (healthFrames == kHealthFrames) {
          const uint32_t rms = static_cast<uint32_t>(sqrt(
              static_cast<double>(healthSquareSum) / static_cast<double>(healthSamples)));
          char detail[160];
          snprintf(detail, sizeof(detail),
                   "frames=%lu samples=%lu peak=%lu rms=%lu nonzero=%lu min=%d max=%d channel=%s",
                   static_cast<unsigned long>(healthFrames), static_cast<unsigned long>(healthSamples),
                   static_cast<unsigned long>(healthPeak), static_cast<unsigned long>(rms),
                   static_cast<unsigned long>(healthNonzero), static_cast<int>(healthMin),
                   static_cast<int>(healthMax), NewoConfig::AUDIO_I2S_MIC_IS_LEFT ? "left" : "right");
          NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_PCM_HEALTH", detail);
        }
      }
      // One 20 ms PCM16 frame is sent directly: no queue and no stale backlog.
      if (!voiceWebSocket_.sendBIN(reinterpret_cast<uint8_t*>(mono), sizeof(mono))) {
        streamEndReason_ = "send_failed";
        break;
      }
    }
    if (static_cast<uint32_t>(millis() - streamStartedMs_) >= NewoConfig::VOICE_ACTIVE_SESSION_TIMEOUT_MS) {
      streamEndReason_ = "timeout"; break;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (!streamEndReason_) streamEndReason_ = stopStreaming_ ? "cancelled" : "disconnected";
  voiceConnected_ = false;
  voiceWebSocket_.disconnect();
  releaseI2s();
  streamFinished_ = true;
#endif
  vTaskDelete(nullptr);
}

void NewoAudio::finishStreaming(const char* reason) {
  if (reason && strcmp(reason, "timeout") == 0) ++timeouts_;
  else if (reason && strcmp(reason, "cancelled") != 0 && strcmp(reason, "final") != 0) ++failures_;
  char detail[48]; snprintf(detail, sizeof(detail), "reason=%s", reason ? reason : "unknown");
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_STREAM_STOPPED", detail);
  streamTask_ = nullptr;
  transitionPending_ = false;
  // Idempotent after the normal task cleanup, and required if task creation
  // failed after direct manual I2S acquisition.
  releaseI2s();
  // LISTENING is session-only; recover the normal face before re-arming/OFF.
  display_.setListeningActive(false);
  if (rearmAfterStream_ && enabled_ && startWakeNet()) return;
  rearmAfterStream_ = false;
  state_ = NewoVoiceState::OFF;
}

void NewoAudio::handleVoiceEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    voiceConnected_ = true;
    NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_CONNECTED");
  } else if (type == WStype_DISCONNECTED) {
    voiceConnected_ = false;
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "VOICE_WS_DISCONNECTED");
    if (!stopStreaming_) { streamEndReason_ = "disconnected"; stopStreaming_ = true; }
  }
  else if (type == WStype_ERROR) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "VOICE_WS_ERROR");
    display_.noteSystemError();
    streamEndReason_ = "socket_error"; stopStreaming_ = true;
  }
  else if (type == WStype_TEXT && length) {
    // Payload is not required to be NUL terminated. Keep parsing allocation-free.
    static constexpr char kFinal[] = "\"type\":\"final\"";
    for (size_t i = 0; i + sizeof(kFinal) - 1 <= length; ++i) {
      if (memcmp(payload + i, kFinal, sizeof(kFinal) - 1) == 0) {
        streamEndReason_ = "final"; stopStreaming_ = true; break;
      }
    }
  }
}

void NewoAudio::loop() {
  if (state_ == NewoVoiceState::ARMED && wakePending_) {
    wakePending_ = false;
    if (beginStreaming(true)) ++wakeCount_;
  }
  if (state_ == NewoVoiceState::STREAMING && streamFinished_) finishStreaming(streamEndReason_);
  if (state_ == NewoVoiceState::OFF && enabled_ && !transitionPending_ && !playbackSuppressed_) startWakeNet();
}
