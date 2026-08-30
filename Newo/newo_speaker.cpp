#include "newo_speaker.h"

#include <ArduinoJson.h>
#include <cstring>
#include <driver/i2s_common.h>
#include <esp_heap_caps.h>

#include "newo_config.h"
#include "newo_log.h"

#if __has_include("newo_secrets.h")
#include "newo_secrets.h"
#define NEWO_SPEAKER_HAS_LOCAL_SECRETS 1
#else
#define NEWO_SPEAKER_HAS_LOCAL_SECRETS 0
#endif

NewoSpeaker::NewoSpeaker(NewoWiFi& wifi, NewoDisplay& display, NewoAudio& audio, NewoStorage& storage)
    : wifi_(wifi), display_(display), audio_(audio), storage_(storage) {}

void NewoSpeaker::begin() {
  volume_ = storage_.speakerVolume();
  muted_ = storage_.speakerMuted();
  enabled_ = storage_.speakerEnabled();
  webSocket_.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
    handleEvent(type, payload, length);
  });
  webSocket_.setReconnectInterval(NewoConfig::CLOUD_RECONNECT_INTERVAL_MS);
  webSocket_.enableHeartbeat(NewoConfig::CLOUD_WS_PING_INTERVAL_MS,
                             NewoConfig::CLOUD_WS_PONG_TIMEOUT_MS,
                             NewoConfig::CLOUD_WS_MISSED_PONG_LIMIT);
  char detail[96];
  snprintf(detail, sizeof(detail), "enabled=%s volume=%u%% muted=%s buffer=%u prebuffer=%u chunk=%u",
           enabled_ ? "yes" : "no", static_cast<unsigned>(volume_), muted_ ? "yes" : "no",
           static_cast<unsigned>(NewoConfig::SPEAKER_BUFFER_BYTES),
           static_cast<unsigned>(NewoConfig::SPEAKER_PREBUFFER_BYTES),
           static_cast<unsigned>(NewoConfig::SPEAKER_CHUNK_BYTES));
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "SPEAKER_READY", detail);
}

NewoSpeaker::MemorySnapshot NewoSpeaker::memorySnapshot() const {
  MemorySnapshot snapshot;
  snapshot.heap = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  snapshot.minimumHeap = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  snapshot.psram = ESP.getFreePsram();
  return snapshot;
}

void NewoSpeaker::logMemory(const char* stage, const MemorySnapshot& snapshot,
                            const MemorySnapshot* comparison) {
  char detail[96];
  if (comparison) {
    snprintf(detail, sizeof(detail), "%s heap=%lu min=%lu psram=%lu d_heap=%ld d_psram=%ld", stage,
             static_cast<unsigned long>(snapshot.heap), static_cast<unsigned long>(snapshot.minimumHeap),
             static_cast<unsigned long>(snapshot.psram),
             static_cast<long>(static_cast<int32_t>(snapshot.heap) - static_cast<int32_t>(comparison->heap)),
             static_cast<long>(static_cast<int32_t>(snapshot.psram) - static_cast<int32_t>(comparison->psram)));
  } else {
    snprintf(detail, sizeof(detail), "%s heap=%lu min=%lu psram=%lu", stage,
             static_cast<unsigned long>(snapshot.heap), static_cast<unsigned long>(snapshot.minimumHeap),
             static_cast<unsigned long>(snapshot.psram));
  }
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "SPEAKER_MEMORY", detail);
}

bool NewoSpeaker::allocateBuffer() {
  if (buffer_) return true;
  buffer_ = xStreamBufferCreate(NewoConfig::SPEAKER_BUFFER_BYTES, 1);
  if (buffer_) return true;
  NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "SPEAKER_BUFFER_FAILED");
  return false;
}

void NewoSpeaker::startConnection() {
#if !NEWO_SPEAKER_HAS_LOCAL_SECRETS
  NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "SPEAKER_CONNECT_FAILED", "secrets_missing");
  return;
#else
  if (started_ || connected_ || !wifi_.connected() || !cloudReady_ ||
      (!enabled_ && !temporaryRequested_)) return;
  beforeConnection_ = memorySnapshot();
  connectedMemory_ = beforeConnection_;
  memoryCycleActive_ = true;
  logMemory("before", beforeConnection_);
  if (!allocateBuffer()) return;

  String headers;
  headers.reserve(strlen(NewoSecrets::DEVICE_ID) + strlen(NewoSecrets::DEVICE_SECRET) + 64);
  headers += F("X-Newo-Device-Id: "); headers += NewoSecrets::DEVICE_ID;
  headers += F("\r\nAuthorization: Bearer "); headers += NewoSecrets::DEVICE_SECRET;
  webSocket_.setExtraHeaders(headers.c_str());
  webSocket_.beginSslWithCA(NewoConfig::CLOUD_HOST, NewoConfig::CLOUD_PORT,
                            NewoConfig::SPEAKER_PATH, NewoSecrets::CLOUD_CA_CERT, "");
  started_ = true;
  releaseRequested_ = false;
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "SPEAKER_CONNECTING",
               temporaryRequested_ && !enabled_ ? "temporary" : "persistent");
#endif
}

void NewoSpeaker::releaseResources() {
  if (playing()) { releaseRequested_ = true; return; }
  if (buffer_) {
    vStreamBufferDelete(buffer_);
    buffer_ = nullptr;
  }
  releaseRequested_ = false;
  if (!memoryCycleActive_) return;
  const MemorySnapshot released = memorySnapshot();
  logMemory("released", released, &beforeConnection_);
  char detail[72];
  snprintf(detail, sizeof(detail), "heap=%ld psram=%ld",
           static_cast<long>(static_cast<int32_t>(released.heap) - static_cast<int32_t>(connectedMemory_.heap)),
           static_cast<long>(static_cast<int32_t>(released.psram) - static_cast<int32_t>(connectedMemory_.psram)));
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
               "SPEAKER_MEMORY_RECOVERED", detail);
  memoryCycleActive_ = false;
}

void NewoSpeaker::stopConnection(const char* reason) {
  if (playing()) fail(reason && reason[0] ? reason : "speaker_disabled");
  if (connected_) webSocket_.disconnect();
  connected_ = false;
  started_ = false;
  if (!playing()) releaseResources();
  else releaseRequested_ = true;
}

bool NewoSpeaker::setEnabled(bool enabled) {
  if (!storage_.setSpeakerEnabled(enabled)) return false;
  enabled_ = enabled;
  if (enabled_) {
    temporaryRequested_ = false;
    reconnectAfterMs_ = 0;
  } else {
    temporaryRequested_ = false;
    stopConnection("speaker_disabled");
  }
  return true;
}

bool NewoSpeaker::requestTemporaryConnection() {
  if (enabled_) return true;
  temporaryRequested_ = true;
  reconnectAfterMs_ = 0;
  return true;
}

const char* NewoSpeaker::connectionStatus() const {
  if (connected_) return "Ready";
  if (started_ || ((enabled_ || temporaryRequested_) && wifi_.connected() && cloudReady_)) return "Connecting";
  return "Disconnected";
}

bool NewoSpeaker::setVolume(uint8_t volume) {
  if (volume > 100 || !storage_.setSpeakerVolume(volume)) return false;
  volume_ = volume;
  return true;
}

bool NewoSpeaker::setMuted(bool muted) {
  if (!storage_.setSpeakerMuted(muted)) return false;
  muted_ = muted;
  return true;
}

const char* NewoSpeaker::lastPlayback() const {
  if (playing()) return "Playing";
  if (lastPlayback_ == LastPlayback::COMPLETE) return "Complete";
  if (lastPlayback_ == LastPlayback::FAILED) return "Failed";
  return "None";
}

bool NewoSpeaker::startPlayback(const Request& request) {
  if (!connected_ || !buffer_ || task_ || resultReady_) return false;
  if (request.sampleRate != NewoConfig::SPEAKER_SAMPLE_RATE || request.channels != 1 ||
      request.bitsPerSample != 16 || request.bytes == 0 ||
      request.bytes > NewoConfig::SPEAKER_MAX_STREAM_BYTES || (request.bytes & 1)) return false;
  if (!audio_.setPlaybackActive(true)) return false;

  request_ = request;
  result_ = {};
  strlcpy(result_.playbackId, request.playbackId, sizeof(result_.playbackId));
  xStreamBufferReset(buffer_);
  endReceived_ = false;
  failed_ = false;
  taskFinished_ = false;
  receivedBytes_ = 0;
  consumedBytes_ = 0;
  firstPcmReceivedMs_ = 0;
  failureReason_ = nullptr;
  minimumTaskStackBytes_ = UINT32_MAX;
  i2sSentEventCount_ = 0;
  lastFlowSentBytes_ = 0;
  flowReportCount_ = 0;
  i2sDrainMs_ = 0;
  underrunCount_ = 0;
  overflowCount_ = 0;
  minimumBufferedBytes_ = UINT32_MAX;
  maximumBufferedBytes_ = 0;
  playbackDurationMs_ = 0;
  playbackStateApplied_ = true;
  playbackStartedEventReady_ = false;
  display_.setSpeaking(true);
  if (xTaskCreatePinnedToCore(taskEntry, "newo-speaker", 8192, this, 2, &task_, 1) != pdPASS) {
    task_ = nullptr;
    display_.setSpeaking(false);
    audio_.setPlaybackActive(false);
    playbackStateApplied_ = false;
    return false;
  }
  return true;
}

void NewoSpeaker::taskEntry(void* context) { static_cast<NewoSpeaker*>(context)->playbackTask(); }

bool IRAM_ATTR NewoSpeaker::onI2sSent(i2s_chan_handle_t handle, i2s_event_data_t* event, void* userData) {
  (void)handle;
  (void)event;
  auto* speaker = static_cast<NewoSpeaker*>(userData);
  if (speaker) ++speaker->i2sSentEventCount_;
  return false;
}

void NewoSpeaker::fail(const char* error) {
  if (!failed_) failureReason_ = error;
  failed_ = true;
}

void NewoSpeaker::handleText(const uint8_t* payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) != DeserializationError::Ok) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::AUDIO, "SPEAKER_INVALID_JSON");
    return;
  }
  const char* type = doc["type"] | "";
  if (strcmp(type, "speaker_begin") == 0) {
    Request request = {};
    strlcpy(request.playbackId, doc["playback_id"] | "", sizeof(request.playbackId));
    request.sampleRate = doc["sample_rate"] | 0;
    request.channels = doc["channels"] | 0;
    request.bitsPerSample = doc["bits_per_sample"] | 0;
    request.bytes = doc["bytes"] | 0;
    if (!request.playbackId[0] || !startPlayback(request)) {
      NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                   "SPEAKER_BEGIN_REJECTED", request.playbackId);
    }
    return;
  }
  if (strcmp(type, "speaker_end") == 0 && playing()) {
    const char* playbackId = doc["playback_id"] | "";
    const uint32_t bytes = doc["bytes"] | 0;
    if (strcmp(playbackId, request_.playbackId) != 0 || bytes != request_.bytes) {
      fail("invalid_end");
    } else {
      endReceived_ = true;
    }
  }
}

void NewoSpeaker::handleEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    connected_ = true;
    started_ = true;
    connectedMemory_ = memorySnapshot();
    logMemory("connected", connectedMemory_, &beforeConnection_);
    webSocket_.sendTXT("{\"type\":\"speaker_ready\"}");
    return;
  }
  if (type == WStype_BIN) {
    if (!playing() || !length || (length & 1) || receivedBytes_ > request_.bytes ||
        length > request_.bytes - receivedBytes_) { fail("invalid_pcm"); return; }
    if (firstPcmReceivedMs_ == 0) firstPcmReceivedMs_ = millis();
    // Never wait in the WebSocket callback. Receiver-driven credit on the VPS
    // keeps unconsumed PCM bounded well below this fixed 16 KiB StreamBuffer.
    if (xStreamBufferSend(buffer_, payload, length, 0) != length) {
      ++overflowCount_;
      fail("buffer_overflow");
      return;
    }
    receivedBytes_ += length;
    return;
  }
  if (type == WStype_TEXT && length) { handleText(payload, length); return; }
  if (type == WStype_DISCONNECTED) {
    const bool wasActive = connected_ || started_;
    connected_ = false;
    started_ = false;
    if (playing() && !endReceived_) fail("disconnected");
    if (!enabled_) temporaryRequested_ = false;
    reconnectAfterMs_ = millis() + NewoConfig::CLOUD_RECONNECT_INTERVAL_MS;
    if (wasActive && !playing() && (!enabled_ || !cloudReady_)) releaseResources();
  } else if (type == WStype_ERROR) {
    if (playing()) fail("socket_error");
  }
}

void NewoSpeaker::sendFlowReport(bool force) {
  if (!connected_ || !playing() || !buffer_) return;
  const uint32_t consumed = consumedBytes_;
  if (consumed <= lastFlowSentBytes_) return;
  if (!force && consumed - lastFlowSentBytes_ < NewoConfig::SPEAKER_FLOW_REPORT_BYTES) return;

  JsonDocument doc;
  doc["type"] = "speaker_flow";
  doc["playback_id"] = request_.playbackId;
  doc["consumed_bytes"] = consumed;
  doc["buffered_bytes"] = static_cast<uint32_t>(xStreamBufferBytesAvailable(buffer_));
  String body;
  serializeJson(doc, body);
  webSocket_.sendTXT(body);
  lastFlowSentBytes_ = consumed;
  ++flowReportCount_;
}

void NewoSpeaker::playbackTask() {
  minimumTaskStackBytes_ = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  i2s_.setPins(NewoConfig::SPEAKER_I2S_BCLK_PIN, NewoConfig::SPEAKER_I2S_WS_PIN,
               NewoConfig::SPEAKER_I2S_DOUT_PIN);
  if (!i2s_.begin(I2S_MODE_STD, NewoConfig::SPEAKER_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                  I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    fail("i2s_failed");
  } else {
    i2s_chan_handle_t txChannel = i2s_.txChan();
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_sent = &NewoSpeaker::onI2sSent;
    if (!txChannel || i2s_channel_disable(txChannel) != ESP_OK ||
        i2s_channel_register_event_callback(txChannel, &callbacks, this) != ESP_OK ||
        i2s_channel_enable(txChannel) != ESP_OK) {
      fail("i2s_callback_failed");
    }

    const uint32_t timeoutMs = 15'000 + (request_.bytes * 1'000UL / NewoConfig::SPEAKER_PCM_BYTES_PER_SECOND);
    const uint32_t startedMs = millis();
    uint32_t playbackStartedMs = 0;
    bool playbackStarted = false;
    bool underrunActive = false;
    while (!failed_) {
      const uint32_t stackBytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
      if (stackBytes < minimumTaskStackBytes_) minimumTaskStackBytes_ = stackBytes;
      size_t available = xStreamBufferBytesAvailable(buffer_);
      if (available > maximumBufferedBytes_) maximumBufferedBytes_ = static_cast<uint32_t>(available);

      if (!playbackStarted) {
        const bool allPcmReceived = receivedBytes_ == request_.bytes;
        if (available >= NewoConfig::SPEAKER_PREBUFFER_BYTES ||
            (allPcmReceived && available >= sizeof(int16_t))) {
          playbackStarted = true;
          playbackStartedMs = millis();
          minimumBufferedBytes_ = static_cast<uint32_t>(available);
          strlcpy(playbackStartedEvent_.playbackId, request_.playbackId,
                  sizeof(playbackStartedEvent_.playbackId));
          playbackStartedEvent_.firstPcmToPlayMs = firstPcmReceivedMs_ == 0
              ? 0 : playbackStartedMs - firstPcmReceivedMs_;
          playbackStartedEventReady_ = true;
          char detail[80];
          snprintf(detail, sizeof(detail), "id=%s first_pcm_to_play_ms=%lu", request_.playbackId,
                   static_cast<unsigned long>(playbackStartedEvent_.firstPcmToPlayMs));
          NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO,
                       "SPEAKER_PLAY_START", detail);
        }
      }

      if (playbackStarted && available >= sizeof(int16_t)) {
        underrunActive = false;
        if (available < minimumBufferedBytes_) minimumBufferedBytes_ = static_cast<uint32_t>(available);
        const size_t evenAvailable = available & ~static_cast<size_t>(1);
        const size_t wanted = min(evenAvailable, sizeof(monoWorking_));
        const size_t count = xStreamBufferReceive(buffer_, monoWorking_, wanted, 0);
        if (count & 1) { fail("unaligned_pcm"); continue; }
        const size_t samples = count / sizeof(int16_t);
        if (samples > kWorkingSamples) { fail("working_overflow"); continue; }
        for (size_t i = 0; i < samples; ++i) {
          const int16_t sample = muted_ ? 0 : static_cast<int16_t>(
              static_cast<int32_t>(monoWorking_[i]) * volume_ /
              (100 * NewoConfig::SPEAKER_DIGITAL_DIVISOR));
          stereoWorking_[i * 2] = sample;
          stereoWorking_[i * 2 + 1] = sample;
        }
        const size_t stereoBytes = samples * 2 * sizeof(int16_t);
        if (stereoBytes > sizeof(stereoWorking_) ||
            i2s_.write(stereoWorking_, stereoBytes) != stereoBytes) {
          fail("i2s_write_failed");
        } else {
          consumedBytes_ += static_cast<uint32_t>(count);
        }
      } else if (playbackStarted && receivedBytes_ < request_.bytes && !underrunActive) {
        ++underrunCount_;
        minimumBufferedBytes_ = 0;
        underrunActive = true;
      }
      if (endReceived_ && xStreamBufferBytesAvailable(buffer_) == 0) {
        if (receivedBytes_ != request_.bytes || consumedBytes_ != request_.bytes) fail("truncated");
        break;
      }
      if (millis() - startedMs >= timeoutMs) { fail("timeout"); break; }
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    // I2SClass::write() only guarantees that PCM was copied into the TX DMA ring.
    // Arduino-ESP32 3.3.11 uses six 240-frame descriptors. Waiting for a full
    // ring plus one additional TX EOF after the final write guarantees the
    // descriptor containing the last audio samples and the hardware FIFO tail
    // have actually drained before SPEAKER_COMPLETE can be emitted.
    if (!failed_ && playbackStarted && endReceived_ && receivedBytes_ == request_.bytes &&
        consumedBytes_ == request_.bytes) {
      const uint32_t drainStartEvents = i2sSentEventCount_;
      const uint32_t drainStartedMs = millis();
      while (static_cast<uint32_t>(i2sSentEventCount_ - drainStartEvents) <
             NewoConfig::SPEAKER_I2S_DRAIN_DMA_EVENTS) {
        if (millis() - drainStartedMs >= NewoConfig::SPEAKER_I2S_DRAIN_TIMEOUT_MS) {
          fail("i2s_drain_timeout");
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      i2sDrainMs_ = millis() - drainStartedMs;
    }

    i2s_.end();
    if (playbackStarted) playbackDurationMs_ = millis() - playbackStartedMs;
  }
  const uint32_t finalStackBytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  if (finalStackBytes < minimumTaskStackBytes_) minimumTaskStackBytes_ = finalStackBytes;
  result_.bytes = receivedBytes_;
  result_.success = !failed_ && endReceived_ && receivedBytes_ == request_.bytes &&
                    consumedBytes_ == request_.bytes;
  strlcpy(result_.error, result_.success ? "" : (failureReason_ ? failureReason_ : "unknown"), sizeof(result_.error));
  taskFinished_ = true;
  vTaskDelete(nullptr);
}

void NewoSpeaker::loop(bool cloudReady) {
  cloudReady_ = cloudReady;
  const bool connectionDesired = enabled_ || temporaryRequested_;
  if ((!wifi_.connected() || !cloudReady_) && (started_ || connected_)) {
    stopConnection("cloud_disconnected");
  } else if (connectionDesired && wifi_.connected() && cloudReady_ && !started_ && !connected_ &&
             static_cast<int32_t>(millis() - reconnectAfterMs_) >= 0) {
    startConnection();
  }
  if (started_) webSocket_.loop();

  if (playing() && connected_ && buffer_) {
    const uint32_t consumed = consumedBytes_;
    const bool finalProgress = endReceived_ && consumed == request_.bytes;
    if (consumed > lastFlowSentBytes_ &&
        (consumed - lastFlowSentBytes_ >= NewoConfig::SPEAKER_FLOW_REPORT_BYTES || finalProgress)) {
      sendFlowReport(finalProgress);
    }
  }

  if (task_ && taskFinished_) {
    task_ = nullptr;
    if (playbackStateApplied_) {
      display_.setSpeaking(false);
      audio_.setPlaybackActive(false);
      playbackStateApplied_ = false;
    }
    resultReady_ = true;
    lastPlayback_ = result_.success ? LastPlayback::COMPLETE : LastPlayback::FAILED;
    char diagnostics[112];
    snprintf(diagnostics, sizeof(diagnostics), "rx=%lu duration_ms=%lu underruns=%lu min_buffer=%lu drain_ms=%lu stack_low=%lu",
             static_cast<unsigned long>(result_.bytes), static_cast<unsigned long>(playbackDurationMs_),
             static_cast<unsigned long>(underrunCount_),
             static_cast<unsigned long>(minimumBufferedBytes_ == UINT32_MAX ? 0 : minimumBufferedBytes_),
             static_cast<unsigned long>(i2sDrainMs_),
             static_cast<unsigned long>(minimumTaskStackBytes_));
    NewoLog::log(underrunCount_ == 0 ? NewoLog::Level::INFO : NewoLog::Level::WARN,
                 NewoLog::Subsystem::AUDIO, "SPEAKER_DIAGNOSTICS", diagnostics);
    char bufferDiagnostics[96];
    snprintf(bufferDiagnostics, sizeof(bufferDiagnostics), "overflows=%lu max_buffer=%lu capacity=%u chunk=%u flow_reports=%lu",
             static_cast<unsigned long>(overflowCount_), static_cast<unsigned long>(maximumBufferedBytes_),
             static_cast<unsigned>(NewoConfig::SPEAKER_BUFFER_BYTES),
             static_cast<unsigned>(NewoConfig::SPEAKER_CHUNK_BYTES),
             static_cast<unsigned long>(flowReportCount_));
    NewoLog::log(overflowCount_ == 0 ? NewoLog::Level::INFO : NewoLog::Level::ERROR,
                 NewoLog::Subsystem::AUDIO, "SPEAKER_BUFFER", bufferDiagnostics);
    NewoLog::log(result_.success ? NewoLog::Level::INFO : NewoLog::Level::ERROR,
                 NewoLog::Subsystem::AUDIO, result_.success ? "SPEAKER_COMPLETE" : "SPEAKER_FAILED",
                 result_.success ? "" : result_.error);
    if (releaseRequested_ || (!enabled_ && !temporaryRequested_)) releaseResources();
  }
}

bool NewoSpeaker::consumePlaybackStarted(PlaybackStarted& started) {
  if (!playbackStartedEventReady_) return false;
  started = playbackStartedEvent_;
  playbackStartedEventReady_ = false;
  return true;
}

bool NewoSpeaker::consumeResult(Result& result) {
  if (!resultReady_) return false;
  result = result_;
  resultReady_ = false;
  return true;
}
