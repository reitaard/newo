#include "newo_audio.h"

#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

#include "newo_log.h"
#include "newo_memory_diagnostics.h"
#include "newo_pcm_ring.h"
#include "newo_webrtc.h"

#if __has_include("newo_secrets.h")
#include "newo_secrets.h"
#define NEWO_AUDIO_HAS_LOCAL_SECRETS 1
#else
#define NEWO_AUDIO_HAS_LOCAL_SECRETS 0
#endif

namespace {
struct VoiceCaptureContext {
  I2SClass* i2s = nullptr;
  NewoPcmFrameRing* ring = nullptr;
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  volatile bool stop = false;
  volatile bool started = false;
  volatile bool finished = false;
  const char* volatile error = nullptr;
  volatile uint32_t startedMs = 0;
};

// This producer deliberately does only microphone capture and bounded storage.
// It never performs DSP or networking, so neither TLS nor NS setup can create a
// hole at the beginning of an utterance.
void voiceCaptureTaskEntry(void* parameter) {
  auto* capture = static_cast<VoiceCaptureContext*>(parameter);
  const uint32_t captureAttemptMs = millis();
  int16_t stereo[NewoConfig::AUDIO_SAMPLES_PER_FRAME * 2];
  int16_t mono[NewoConfig::AUDIO_SAMPLES_PER_FRAME];

  while (!capture->stop) {
    if (capture->i2s->readBytes(reinterpret_cast<char*>(stereo), sizeof(stereo)) != sizeof(stereo)) {
      if (!capture->stop) capture->error = "i2s_read_failed";
      break;
    }
    for (size_t i = 0; i < NewoConfig::AUDIO_SAMPLES_PER_FRAME; ++i) {
      mono[i] = stereo[i * 2 + (NewoConfig::AUDIO_I2S_MIC_IS_LEFT ? 0 : 1)];
    }

    portENTER_CRITICAL(&capture->mux);
    capture->ring->push(mono);
    portEXIT_CRITICAL(&capture->mux);

    // Signal readiness only after the first complete PCM frame is safely in the
    // ring. The network consumer cannot start TLS before this point.
    if (!capture->started) {
      capture->startedMs = captureAttemptMs;
      capture->started = true;
    }
  }

  capture->finished = true;
  vTaskDeleteWithCaps(nullptr);
}
}  // namespace

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
  i2s_.setTimeout(NewoConfig::AUDIO_I2S_READ_TIMEOUT_MS);
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
  // Streaming shortens the read timeout so its producer can be joined quickly.
  // Restore Arduino Stream's normal 1 s timeout before handing I2S to WakeNet.
  i2s_.setTimeout(1'000);
  NewoMemoryDiagnostics::log("BEFORE_WAKENET_START");
  const bool started = wakeEngine_.start(i2s_, srEvent);
  NewoMemoryDiagnostics::log("AFTER_WAKENET_START");
  if (!started) {
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
  // read when the streaming capture task is created.
  wakeEngine_.stop();
  NewoMemoryDiagnostics::log("AFTER_WAKENET_STOP");
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
  // A future WakeNet event and a manual request share this stream session.
  // StopWakeNet also releases its I2S ownership before direct capture begins.
  stopWakeNet();
  if (!configureI2s()) { ++failures_; return false; }
  rearmAfterStream_ = rearmAfterStream;
  if (!rearmAfterStream_) enabled_ = false;
  state_ = NewoVoiceState::STREAMING;
  transitionPending_ = true;
  streamFinished_ = false;
  stopStreaming_ = false;
  streamEndReason_ = nullptr;
  streamStartedMs_ = millis();
  ++sessionCount_;
  // Network/TLS gets its own task on core 0. A separate higher-priority capture
  // producer on core 1 owns I2S, so synchronous connect/write cannot stop PCM.
  if (xTaskCreatePinnedToCore(streamTaskEntry, "newo-voice-net", 8192, this, 2, &streamTask_, 0) != pdPASS) {
    ++failures_;
    streamEndReason_ = "task_failed";
    streamFinished_ = true;
    display_.setListeningActive(false);
    display_.noteSystemError();
    return false;
  }
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

bool NewoAudio::startPhysicalVoiceTrigger() {
  if (state_ == NewoVoiceState::STREAMING || streamTask_) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO,
                 "PHYSICAL_TRIGGER_REJECTED", "reason=voice_active");
    return false;
  }
  if (playbackSuppressed_) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO,
                 "PHYSICAL_TRIGGER_REJECTED", "reason=speaker_busy");
    return false;
  }
  if (!beginStreaming(false)) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO,
                 "PHYSICAL_TRIGGER_REJECTED", "reason=unavailable");
    return false;
  }
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
               "PHYSICAL_TRIGGER_ACCEPTED");
  return true;
}

void NewoAudio::streamTaskEntry(void* context) { static_cast<NewoAudio*>(context)->streamTask(); }

void NewoAudio::streamTask() {
#if NEWO_AUDIO_HAS_LOCAL_SECRETS
  if (!configureI2s()) {
    streamEndReason_ = "i2s_failed";
    streamFinished_ = true;
    vTaskDelete(nullptr);
    return;
  }

  int16_t* captureStorage = static_cast<int16_t*>(heap_caps_malloc(
      NewoConfig::VOICE_CAPTURE_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  int16_t* rawBatch = static_cast<int16_t*>(heap_caps_malloc(
      NewoConfig::VOICE_TX_MAX_BATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  int16_t* txBatch = static_cast<int16_t*>(heap_caps_malloc(
      NewoConfig::VOICE_TX_MAX_BATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!captureStorage || !rawBatch || !txBatch) {
    if (captureStorage) heap_caps_free(captureStorage);
    if (rawBatch) heap_caps_free(rawBatch);
    if (txBatch) heap_caps_free(txBatch);
    streamEndReason_ = "capture_buffer_failed";
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "VOICE_CAPTURE_BUFFER_FAILED");
    releaseI2s();
    streamFinished_ = true;
    vTaskDelete(nullptr);
    return;
  }

  NewoPcmFrameRing ring(captureStorage, NewoConfig::VOICE_CAPTURE_BUFFER_FRAMES,
                        NewoConfig::AUDIO_SAMPLES_PER_FRAME);
  VoiceCaptureContext capture;
  capture.i2s = &i2s_;
  capture.ring = &ring;
  if (xTaskCreatePinnedToCoreWithCaps(
          voiceCaptureTaskEntry, "newo-voice-rx", NewoConfig::VOICE_CAPTURE_TASK_STACK_BYTES,
          &capture, 3, nullptr, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    heap_caps_free(txBatch);
    heap_caps_free(rawBatch);
    heap_caps_free(captureStorage);
    streamEndReason_ = "capture_task_failed";
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                 "VOICE_CAPTURE_TASK_FAILED");
    releaseI2s();
    streamFinished_ = true;
    vTaskDelete(nullptr);
    return;
  }

  // Do not initialize DSP or start TLS until one complete microphone frame is
  // already retained. Speech can begin immediately after the trigger.
  while (!capture.started && !capture.finished) vTaskDelay(pdMS_TO_TICKS(1));
  if (capture.finished && capture.error) streamEndReason_ = capture.error;

  NewoWebRtcHandle* voiceDsp = nullptr;
  if (!streamEndReason_ && NewoConfig::VOICE_WEBRTC_NS_ENABLED) {
    voiceDsp = webrtc_create(NewoConfig::AUDIO_FRAME_DURATION_MS,
                             NewoConfig::VOICE_WEBRTC_NS_MODE,
                             AGC_MODE_SR, 9, -3, NewoConfig::AUDIO_SAMPLE_RATE);
    if (!voiceDsp) {
      streamEndReason_ = "ns_create_failed";
      NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                   "VOICE_NS_FAILED", "reason=create_failed");
    } else {
      char detail[96];
      snprintf(detail, sizeof(detail), "mode=%d agc=%s frame_ms=%u sample_rate=%lu",
               static_cast<int>(NewoConfig::VOICE_WEBRTC_NS_MODE),
               NewoConfig::VOICE_WEBRTC_AGC_ENABLED ? "on" : "off",
               static_cast<unsigned>(NewoConfig::AUDIO_FRAME_DURATION_MS),
               static_cast<unsigned long>(NewoConfig::AUDIO_SAMPLE_RATE));
      NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
                   "VOICE_NS_READY", detail);
    }
  }

  String headers;
  if (!streamEndReason_) {
    headers.reserve(strlen(NewoSecrets::DEVICE_ID) + strlen(NewoSecrets::DEVICE_SECRET) + 64);
    headers += F("X-Newo-Device-Id: "); headers += NewoSecrets::DEVICE_ID;
    headers += F("\r\nAuthorization: Bearer "); headers += NewoSecrets::DEVICE_SECRET;
    voiceWebSocket_.setExtraHeaders(headers.c_str());
    NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_CONNECTING");
    voiceWebSocket_.beginSslWithCA(NewoConfig::CLOUD_HOST, NewoConfig::CLOUD_PORT,
                                   NewoConfig::VOICE_PATH, NewoSecrets::CLOUD_CA_CERT, "");
  }

  constexpr uint32_t kHealthFrames = 25;  // First 0.5 s of transmitted audio.
  uint32_t healthFrames = 0;
  uint32_t healthSamples = 0;
  uint32_t healthNonzero = 0;
  uint32_t healthPeak = 0;
  uint64_t healthSquareSum = 0;
  int16_t healthMin = 32767;
  int16_t healthMax = -32768;
  uint32_t txHealthPeak = 0;
  uint64_t txHealthSquareSum = 0;
  bool prerollLogged = false;
  uint32_t reportedOverwrittenFrames = 0;
  uint32_t transmittedFrames = 0;

  while (!stopStreaming_ && !streamEndReason_) {
    // WebSockets 2.7.2 may block here for TCP/TLS. I2S capture is deliberately
    // on another task and keeps filling the bounded PSRAM ring while it blocks.
    voiceWebSocket_.loop();
    if (stopStreaming_ || streamEndReason_) break;

    if (capture.finished) {
      streamEndReason_ = capture.error ? capture.error : "capture_stopped";
      break;
    }
    if (static_cast<uint32_t>(millis() - streamStartedMs_) >= NewoConfig::VOICE_ACTIVE_SESSION_TIMEOUT_MS) {
      streamEndReason_ = "timeout";
      break;
    }
    if (!voiceConnected_) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    if (!prerollLogged) {
      size_t bufferedFrames = 0;
      uint32_t overwrittenFrames = 0;
      portENTER_CRITICAL(&capture.mux);
      bufferedFrames = ring.size();
      overwrittenFrames = ring.overwrittenFrames();
      portEXIT_CRITICAL(&capture.mux);
      const uint32_t captureStartMs = static_cast<uint32_t>(capture.startedMs - streamStartedMs_);
      char detail[208];
      snprintf(detail, sizeof(detail),
               "connect_ms=%lu capture_start_ms=%lu buffered_frames=%lu buffered_ms=%lu overwritten_frames=%lu ring_ms=%lu psram=yes",
               static_cast<unsigned long>(millis() - streamStartedMs_),
               static_cast<unsigned long>(captureStartMs),
               static_cast<unsigned long>(bufferedFrames),
               static_cast<unsigned long>(bufferedFrames * NewoConfig::AUDIO_FRAME_DURATION_MS),
               static_cast<unsigned long>(overwrittenFrames),
               static_cast<unsigned long>(NewoConfig::VOICE_CAPTURE_BUFFER_MS));
      NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "VOICE_PREROLL", detail);
      reportedOverwrittenFrames = overwrittenFrames;
      prerollLogged = true;
    }

    size_t frames = 0;
    size_t queuedAfter = 0;
    uint32_t overwrittenFrames = 0;
    portENTER_CRITICAL(&capture.mux);
    frames = ring.pop(rawBatch, NewoConfig::VOICE_TX_MAX_BATCH_FRAMES);
    queuedAfter = ring.size();
    overwrittenFrames = ring.overwrittenFrames();
    portEXIT_CRITICAL(&capture.mux);

    if (overwrittenFrames > reportedOverwrittenFrames) {
      char detail[128];
      snprintf(detail, sizeof(detail), "total=%lu delta=%lu queued_ms=%lu",
               static_cast<unsigned long>(overwrittenFrames),
               static_cast<unsigned long>(overwrittenFrames - reportedOverwrittenFrames),
               static_cast<unsigned long>(queuedAfter * NewoConfig::AUDIO_FRAME_DURATION_MS));
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO,
                   "VOICE_CAPTURE_OVERWRITE", detail);
      reportedOverwrittenFrames = overwrittenFrames;
    }

    if (frames == 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    size_t processedFrames = 0;
    for (size_t frame = 0; frame < frames; ++frame) {
      int16_t* rawFrame = rawBatch + frame * NewoConfig::AUDIO_SAMPLES_PER_FRAME;
      const bool collectHealth = healthFrames < kHealthFrames;
      if (collectHealth) {
        for (size_t i = 0; i < NewoConfig::AUDIO_SAMPLES_PER_FRAME; ++i) {
          const int32_t sample = rawFrame[i];
          const uint32_t absolute = sample < 0 ? static_cast<uint32_t>(-sample) : static_cast<uint32_t>(sample);
          if (sample != 0) ++healthNonzero;
          if (absolute > healthPeak) healthPeak = absolute;
          if (sample < healthMin) healthMin = static_cast<int16_t>(sample);
          if (sample > healthMax) healthMax = static_cast<int16_t>(sample);
          healthSquareSum += static_cast<uint64_t>(sample * sample);
        }
      }

      int16_t* processed = rawFrame;
      if (voiceDsp) {
        int outputSamples = 0;
        processed = webrtc_process(voiceDsp, rawFrame, &outputSamples,
                                   true, NewoConfig::VOICE_WEBRTC_AGC_ENABLED);
        if (!processed || outputSamples != static_cast<int>(NewoConfig::AUDIO_SAMPLES_PER_FRAME)) {
          char detail[80];
          snprintf(detail, sizeof(detail), "samples=%d expected=%lu", outputSamples,
                   static_cast<unsigned long>(NewoConfig::AUDIO_SAMPLES_PER_FRAME));
          NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                       "VOICE_NS_FAILED", detail);
          streamEndReason_ = "ns_process_failed";
          break;
        }
      }
      memcpy(txBatch + frame * NewoConfig::AUDIO_SAMPLES_PER_FRAME,
             processed, NewoConfig::AUDIO_FRAME_BYTES);
      ++processedFrames;

      if (collectHealth) {
        for (size_t i = 0; i < NewoConfig::AUDIO_SAMPLES_PER_FRAME; ++i) {
          const int32_t sample = processed[i];
          const uint32_t absolute = sample < 0 ? static_cast<uint32_t>(-sample) : static_cast<uint32_t>(sample);
          if (absolute > txHealthPeak) txHealthPeak = absolute;
          txHealthSquareSum += static_cast<uint64_t>(sample * sample);
        }
        ++healthFrames;
        healthSamples += NewoConfig::AUDIO_SAMPLES_PER_FRAME;
        if (healthFrames == kHealthFrames) {
          const uint32_t rms = static_cast<uint32_t>(sqrt(
              static_cast<double>(healthSquareSum) / static_cast<double>(healthSamples)));
          const uint32_t txRms = static_cast<uint32_t>(sqrt(
              static_cast<double>(txHealthSquareSum) / static_cast<double>(healthSamples)));
          char detail[224];
          snprintf(detail, sizeof(detail),
                   "frames=%lu samples=%lu peak=%lu rms=%lu nonzero=%lu min=%d max=%d channel=%s tx_peak=%lu tx_rms=%lu ns=%s",
                   static_cast<unsigned long>(healthFrames), static_cast<unsigned long>(healthSamples),
                   static_cast<unsigned long>(healthPeak), static_cast<unsigned long>(rms),
                   static_cast<unsigned long>(healthNonzero), static_cast<int>(healthMin),
                   static_cast<int>(healthMax), NewoConfig::AUDIO_I2S_MIC_IS_LEFT ? "left" : "right",
                   static_cast<unsigned long>(txHealthPeak), static_cast<unsigned long>(txRms),
                   voiceDsp ? "webrtc" : "raw");
          NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
                       "VOICE_PCM_HEALTH", detail);
        }
      }
    }
    if (streamEndReason_) break;
    if (processedFrames == 0) continue;

    const uint32_t sendStartedMs = millis();
    if (!voiceWebSocket_.sendBIN(reinterpret_cast<uint8_t*>(txBatch),
                                 processedFrames * NewoConfig::AUDIO_FRAME_BYTES)) {
      streamEndReason_ = "send_failed";
      break;
    }
    transmittedFrames += static_cast<uint32_t>(processedFrames);
    const uint32_t sendMs = static_cast<uint32_t>(millis() - sendStartedMs);
    if (sendMs >= NewoConfig::VOICE_TX_STALL_LOG_MS) {
      char detail[128];
      snprintf(detail, sizeof(detail), "send_ms=%lu frames=%lu queued_ms=%lu",
               static_cast<unsigned long>(sendMs), static_cast<unsigned long>(processedFrames),
               static_cast<unsigned long>(queuedAfter * NewoConfig::AUDIO_FRAME_DURATION_MS));
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO,
                   "VOICE_TX_STALL", detail);
    }
  }

  if (!streamEndReason_) streamEndReason_ = stopStreaming_ ? "cancelled" : "disconnected";
  capture.stop = true;
  // readBytes uses a 250 ms timeout, so joining the producer is bounded in normal
  // operation. Never free its stack context/ring storage while it still owns them.
  while (!capture.finished) vTaskDelay(pdMS_TO_TICKS(1));
  if (voiceDsp) webrtc_destroy(voiceDsp);

  size_t remainingFrames = 0;
  uint32_t pushedFrames = 0;
  uint32_t poppedFrames = 0;
  uint32_t overwrittenFrames = 0;
  portENTER_CRITICAL(&capture.mux);
  remainingFrames = ring.size();
  pushedFrames = ring.pushedFrames();
  poppedFrames = ring.poppedFrames();
  overwrittenFrames = ring.overwrittenFrames();
  portEXIT_CRITICAL(&capture.mux);
  char summary[208];
  snprintf(summary, sizeof(summary),
           "captured=%lu dequeued=%lu sent=%lu remaining=%lu overwritten=%lu remaining_ms=%lu",
           static_cast<unsigned long>(pushedFrames), static_cast<unsigned long>(poppedFrames),
           static_cast<unsigned long>(transmittedFrames), static_cast<unsigned long>(remainingFrames),
           static_cast<unsigned long>(overwrittenFrames),
           static_cast<unsigned long>(remainingFrames * NewoConfig::AUDIO_FRAME_DURATION_MS));
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
               "VOICE_CAPTURE_SUMMARY", summary);

  voiceConnected_ = false;
  voiceWebSocket_.disconnect();
  releaseI2s();
  heap_caps_free(txBatch);
  heap_caps_free(rawBatch);
  heap_caps_free(captureStorage);
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
  display_.setListeningActive(state_ == NewoVoiceState::STREAMING && voiceConnected_);
  if (state_ == NewoVoiceState::ARMED && wakePending_) {
    wakePending_ = false;
    if (beginStreaming(true)) ++wakeCount_;
  }
  if (state_ == NewoVoiceState::STREAMING && streamFinished_) finishStreaming(streamEndReason_);
  if (state_ == NewoVoiceState::OFF && enabled_ && !transitionPending_ && !playbackSuppressed_) startWakeNet();
}
