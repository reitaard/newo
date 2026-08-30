#include "newo_speaker.h"

#include <ArduinoJson.h>
#include <cstring>
#include <driver/i2s_common.h>
#include <esp_heap_caps.h>

#include "newo_config.h"
#include "newo_log.h"
#include "newo_speaker_protocol.h"

#if __has_include("newo_secrets.h")
#include "newo_secrets.h"
#define NEWO_SPEAKER_HAS_LOCAL_SECRETS 1
#else
#define NEWO_SPEAKER_HAS_LOCAL_SECRETS 0
#endif

namespace {
constexpr uint8_t kOpusMagic[4] = {'N', 'W', 'O', 'P'};

uint16_t readLe16(const uint8_t* value) {
  return static_cast<uint16_t>(value[0]) |
         (static_cast<uint16_t>(value[1]) << 8);
}
}  // namespace

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
  char detail[112];
  snprintf(detail, sizeof(detail), "enabled=%s volume=%u%% muted=%s buffer=%u prebuffer=%u chunk=%u codecs=pcm,opus",
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

void NewoSpeaker::releaseOpusDecoder() {
  if (!opusDecoder_) return;
  opus_decoder_destroy(opusDecoder_);
  opusDecoder_ = nullptr;
}

bool NewoSpeaker::allocateOpusQueue() {
  if (opusReadyQueue_) return true;
  opusReadyQueue_ = xQueueCreate(NewoConfig::SPEAKER_OPUS_QUEUE_DEPTH, sizeof(OpusPacketRef));
  opusFreeQueue_ = xQueueCreate(NewoConfig::SPEAKER_OPUS_QUEUE_DEPTH, sizeof(uint8_t));
  opusPacketStorage_ = static_cast<uint8_t*>(heap_caps_malloc(
      NewoConfig::SPEAKER_OPUS_QUEUE_DEPTH * NewoConfig::SPEAKER_OPUS_PACKET_MAX_BYTES,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!opusReadyQueue_ || !opusFreeQueue_ || !opusPacketStorage_) { releaseOpusQueue(); return false; }
  return true;
}

bool NewoSpeaker::resetOpusQueue() {
  if (decoderTask_ || !opusReadyQueue_ || !opusFreeQueue_) return false;
  xQueueReset(opusReadyQueue_);
  xQueueReset(opusFreeQueue_);
  for (uint8_t slot = 0; slot < NewoConfig::SPEAKER_OPUS_QUEUE_DEPTH; ++slot) {
    if (xQueueSend(opusFreeQueue_, &slot, 0) != pdTRUE) return false;
  }
  const bool valid = uxQueueMessagesWaiting(opusReadyQueue_) == 0 &&
      uxQueueMessagesWaiting(opusFreeQueue_) == NewoConfig::SPEAKER_OPUS_QUEUE_DEPTH;
  if (!valid) NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "SPEAKER_OPUS_QUEUE_INVALID");
  return valid;
}

void NewoSpeaker::releaseOpusQueue() {
  if (decoderTask_) return;
  if (opusReadyQueue_) { vQueueDelete(opusReadyQueue_); opusReadyQueue_ = nullptr; }
  if (opusFreeQueue_) { vQueueDelete(opusFreeQueue_); opusFreeQueue_ = nullptr; }
  if (opusPacketStorage_) { heap_caps_free(opusPacketStorage_); opusPacketStorage_ = nullptr; }
}

void NewoSpeaker::releaseResources() {
  if (playing()) { releaseRequested_ = true; return; }
  if (decoderTask_) { releaseRequested_ = true; return; }
  releaseOpusDecoder();
  releaseOpusQueue();
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
  if (started_ || connected_) webSocket_.disconnect();
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
  if (!connected_ || !buffer_ || task_ || decoderTask_ || taskFinished_ || resultReady_) return false;
  if (!newoValidSpeakerBegin(request.sampleRate, request.channels, request.bitsPerSample,
                             request.streaming, request.bytes, request.maxBytes,
                             NewoConfig::SPEAKER_SAMPLE_RATE,
                             NewoConfig::SPEAKER_MAX_STREAM_BYTES)) return false;
  if (request.codec == Codec::OPUS &&
      (request.opusFrameMs != NewoConfig::SPEAKER_OPUS_FRAME_MS ||
       request.opusFramePcmBytes != NewoConfig::SPEAKER_OPUS_FRAME_PCM_BYTES)) return false;

  releaseOpusDecoder();
  if (request.codec == Codec::OPUS && (!allocateOpusQueue() || !resetOpusQueue())) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "SPEAKER_OPUS_INIT_FAILED", "queue_allocation");
    publishStartupFailure(request, "opus_queue_allocation_failed");
    return false;
  }
  if (!audio_.setPlaybackActive(true)) {
    releaseOpusDecoder();
    publishStartupFailure(request, "playback_activation_failed");
    return false;
  }

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
  lastPcmReceivedMs_ = millis();
  failureReason_ = nullptr;
  minimumTaskStackBytes_ = UINT32_MAX;
  i2sSentEventCount_ = 0;
  lastFlowSentReceivedBytes_ = 0;
  lastFlowSentConsumedBytes_ = 0;
  lastFlowReportMs_ = millis();
  flowReportCount_ = 0;
  receivedFlowReportCount_ = 0;
  i2sDrainMs_ = 0;
  underrunCount_ = 0;
  overflowCount_ = 0;
  minimumBufferedBytes_ = UINT32_MAX;
  maximumBufferedBytes_ = 0;
  playbackDurationMs_ = 0;
  opusPacketsReceived_ = 0;
  opusBytesReceived_ = 0;
  opusDecodeCount_ = 0;
  opusDecoderErrors_ = 0;
  opusDecodeTotalUs_ = 0;
  opusDecodeWorstUs_ = 0;
  expectedOpusSequence_ = 0;
  expectedOpusDecodeSequence_ = 0;
  opusAdmittedBytes_ = 0;
  decoderFinished_ = request.codec != Codec::OPUS;
  decoderAbort_ = false;
  opusQueueHighWaterPackets_ = 0;
  opusQueueHighWaterBytes_ = 0;
  opusQueueOverflows_ = 0;
  opusQueuedWireBytes_ = 0;
  opusCallbackCount_ = 0;
  opusCallbackTotalUs_ = 0;
  opusCallbackWorstUs_ = 0;
  minimumDecoderStackBytes_ = UINT32_MAX;
  if (request.codec == Codec::OPUS && xTaskCreatePinnedToCore(decoderTaskEntry, "newo-opus", NewoConfig::SPEAKER_OPUS_DECODER_STACK_BYTES, this, 1, &decoderTask_, 1) != pdPASS) {
    decoderTask_ = nullptr; audio_.setPlaybackActive(false); releaseOpusQueue();
    publishStartupFailure(request, "opus_decoder_task_create_failed");
    return false;
  }
  opusSawPartialFrame_ = false;
  playbackStateApplied_ = true;
  playbackStartedEventReady_ = false;
  playbackStarted_ = false;
  display_.setSpeaking(true);
  if (xTaskCreatePinnedToCore(taskEntry, "newo-speaker", 8192, this, 2, &task_, 1) != pdPASS) {
    task_ = nullptr;
    display_.setSpeaking(false);
    audio_.setPlaybackActive(false);
    playbackStateApplied_ = false;
    // The decoder task exclusively owns opusDecoder_ and queue slots.
    fail("playback_task_create_failed");
    result_ = {};
    strlcpy(result_.playbackId, request.playbackId, sizeof(result_.playbackId));
    result_.success = false;
    strlcpy(result_.error, "playback_task_create_failed", sizeof(result_.error));
    decoderAbort_ = true;
    taskFinished_ = true;
    return false;
  }
  return true;
}

void NewoSpeaker::taskEntry(void* context) { static_cast<NewoSpeaker*>(context)->playbackTask(); }
void NewoSpeaker::decoderTaskEntry(void* context) { static_cast<NewoSpeaker*>(context)->opusDecoderTask(); }

void NewoSpeaker::opusDecoderTask() {
  minimumDecoderStackBytes_ = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  int opusError = OPUS_OK;
  opusDecoder_ = opus_decoder_create(NewoConfig::SPEAKER_SAMPLE_RATE, 1, &opusError);
  if (!opusDecoder_ || opusError != OPUS_OK) fail("opus_decoder_init");
  OpusPacketRef packet;
  while (!failed_ && !decoderAbort_) {
    if (xQueueReceive(opusReadyQueue_, &packet, pdMS_TO_TICKS(10)) != pdTRUE) {
      if (endReceived_ && uxQueueMessagesWaiting(opusReadyQueue_) == 0) break;
      continue;
    }
    const uint8_t* encoded = opusPacketStorage_ + static_cast<size_t>(packet.slot) * NewoConfig::SPEAKER_OPUS_PACKET_MAX_BYTES;
    portENTER_CRITICAL(&stateMux_);
    if (opusQueuedWireBytes_ < packet.length) { portEXIT_CRITICAL(&stateMux_); fail("opus_queue_accounting"); break; }
    opusQueuedWireBytes_ -= packet.length;
    portEXIT_CRITICAL(&stateMux_);
    const uint32_t started = micros();
    const int samples = opus_decode(opusDecoder_, encoded + NewoConfig::SPEAKER_OPUS_PACKET_HEADER_BYTES,
      packet.length - NewoConfig::SPEAKER_OPUS_PACKET_HEADER_BYTES, opusDecoded_, NewoConfig::SPEAKER_OPUS_FRAME_SAMPLES, 0);
    const uint32_t elapsed = micros() - started;
    ++opusDecodeCount_; opusDecodeTotalUs_ += elapsed; if (elapsed > opusDecodeWorstUs_) opusDecodeWorstUs_ = elapsed;
    if (samples != static_cast<int>(NewoConfig::SPEAKER_OPUS_FRAME_SAMPLES) || packet.sequence != expectedOpusDecodeSequence_ ||
        xStreamBufferSend(buffer_, reinterpret_cast<const uint8_t*>(opusDecoded_), packet.validPcmBytes, pdMS_TO_TICKS(100)) != packet.validPcmBytes) {
      ++opusDecoderErrors_; fail(samples < 0 ? "opus_decode_failed" : "opus_decode_output");
    } else {
      ++expectedOpusDecodeSequence_; receivedBytes_ += packet.validPcmBytes;
      if (firstPcmReceivedMs_ == 0) firstPcmReceivedMs_ = millis(); lastPcmReceivedMs_ = millis();
    }
    xQueueSend(opusFreeQueue_, &packet.slot, 0);
    const uint32_t stack = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    if (stack < minimumDecoderStackBytes_) minimumDecoderStackBytes_ = stack;
  }
  releaseOpusDecoder();
  decoderFinished_ = true;
  decoderTask_ = nullptr;
  vTaskDelete(nullptr);
}

bool IRAM_ATTR NewoSpeaker::onI2sSent(i2s_chan_handle_t handle, i2s_event_data_t* event, void* userData) {
  (void)handle;
  (void)event;
  auto* speaker = static_cast<NewoSpeaker*>(userData);
  if (speaker) ++speaker->i2sSentEventCount_;
  return false;
}

void NewoSpeaker::fail(const char* error) {
  portENTER_CRITICAL(&stateMux_);
  if (!failed_) failureReason_ = error;
  failed_ = true;
  portEXIT_CRITICAL(&stateMux_);
}

void NewoSpeaker::publishStartupFailure(const Request& request, const char* error) {
  result_ = {};
  strlcpy(result_.playbackId, request.playbackId, sizeof(result_.playbackId));
  result_.success = false;
  strlcpy(result_.error, error, sizeof(result_.error));
  resultReady_ = true;
  lastPlayback_ = LastPlayback::FAILED;
  NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "SPEAKER_FAILED", error);
}

bool NewoSpeaker::handleOpusPacket(const uint8_t* payload, size_t length) {
  const uint32_t limitBytes = request_.streaming ? request_.maxBytes : request_.bytes;
  if (!opusReadyQueue_ || !opusFreeQueue_ || !opusPacketStorage_ || endReceived_ ||
      length <= NewoConfig::SPEAKER_OPUS_PACKET_HEADER_BYTES || length > NewoConfig::SPEAKER_OPUS_PACKET_MAX_BYTES) {
    fail("invalid_opus");
    return false;
  }
  if (memcmp(payload, kOpusMagic, sizeof(kOpusMagic)) != 0) {
    fail("invalid_opus_magic");
    return false;
  }
  const uint16_t sequence = readLe16(payload + 4);
  const uint16_t validPcmBytes = readLe16(payload + 6);
  if (sequence != expectedOpusSequence_) {
    fail("opus_sequence");
    return false;
  }
  if (opusSawPartialFrame_) {
    fail("opus_after_partial");
    return false;
  }
  if (validPcmBytes == 0 || (validPcmBytes & 1) ||
      validPcmBytes > NewoConfig::SPEAKER_OPUS_FRAME_PCM_BYTES ||
      opusAdmittedBytes_ > limitBytes || validPcmBytes > limitBytes - opusAdmittedBytes_) {
    fail("invalid_opus_pcm");
    return false;
  }

  uint8_t slot;
  if (xQueueReceive(opusFreeQueue_, &slot, 0) != pdTRUE) { ++opusQueueOverflows_; fail("opus_queue_overflow"); return false; }
  memcpy(opusPacketStorage_ + static_cast<size_t>(slot) * NewoConfig::SPEAKER_OPUS_PACKET_MAX_BYTES, payload, length);
  const OpusPacketRef packet = {slot, static_cast<uint16_t>(length), sequence, validPcmBytes};
  // Account before making the descriptor visible: a same-core decoder may run
  // immediately when xQueueSend wakes it.
  portENTER_CRITICAL(&stateMux_);
  ++expectedOpusSequence_;
  ++opusPacketsReceived_;
  opusBytesReceived_ += static_cast<uint32_t>(length);
  opusAdmittedBytes_ += validPcmBytes;
  opusQueuedWireBytes_ += length;
  if (opusQueuedWireBytes_ > opusQueueHighWaterBytes_) opusQueueHighWaterBytes_ = opusQueuedWireBytes_;
  if (validPcmBytes < NewoConfig::SPEAKER_OPUS_FRAME_PCM_BYTES) opusSawPartialFrame_ = true;
  portEXIT_CRITICAL(&stateMux_);
  if (xQueueSend(opusReadyQueue_, &packet, 0) != pdTRUE) {
    portENTER_CRITICAL(&stateMux_);
    --expectedOpusSequence_; --opusPacketsReceived_; opusBytesReceived_ -= length;
    opusAdmittedBytes_ -= validPcmBytes; opusQueuedWireBytes_ -= length;
    portEXIT_CRITICAL(&stateMux_);
    xQueueSend(opusFreeQueue_, &slot, 0); ++opusQueueOverflows_; fail("opus_queue_overflow"); return false;
  }
  const uint32_t queued = uxQueueMessagesWaiting(opusReadyQueue_);
  if (queued > opusQueueHighWaterPackets_) opusQueueHighWaterPackets_ = queued;
  return true;
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
    request.streaming = doc["streaming"] | false;
    request.bytes = doc["bytes"] | 0;
    request.maxBytes = request.streaming
        ? static_cast<uint32_t>(doc["max_bytes"] | 0)
        : request.bytes;
    const char* codec = doc["codec"] | "pcm";
    if (strcmp(codec, "pcm") == 0) {
      request.codec = Codec::PCM;
    } else if (strcmp(codec, "opus") == 0) {
      request.codec = Codec::OPUS;
      request.opusFrameMs = doc["opus_frame_ms"] | 0;
      request.opusFramePcmBytes = doc["opus_frame_pcm_bytes"] | 0;
    } else {
      NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                   "SPEAKER_BEGIN_REJECTED", "unsupported_codec");
      return;
    }
    if (!request.playbackId[0] || !startPlayback(request)) {
      NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO,
                   "SPEAKER_BEGIN_REJECTED", request.playbackId);
    }
    return;
  }
  if (strcmp(type, "speaker_end") == 0 && playing() && !taskFinished_) {
    const char* playbackId = doc["playback_id"] | "";
    const uint32_t bytes = doc["bytes"] | 0;
    const NewoSpeakerEndValidation validation = newoValidateSpeakerEnd(
        strcmp(playbackId, request_.playbackId) == 0, request_.streaming, bytes,
        request_.codec == Codec::OPUS ? opusAdmittedBytes_ : receivedBytes_, request_.bytes, request_.maxBytes);
    if (validation == NewoSpeakerEndValidation::WRONG_PLAYBACK_ID) fail("wrong_playback_id");
    else if (validation != NewoSpeakerEndValidation::OK) fail("invalid_end");
    else { request_.bytes = bytes; endReceived_ = true; }
  }
}

void NewoSpeaker::handleEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    // A late TLS/WebSocket completion must not resurrect a disabled speaker.
    if (!enabled_ && !temporaryRequested_) { webSocket_.disconnect(); started_ = false; return; }
    connected_ = true;
    started_ = true;
    connectedMemory_ = memorySnapshot();
    logMemory("connected", connectedMemory_, &beforeConnection_);
    webSocket_.sendTXT("{\"type\":\"speaker_ready\",\"codecs\":[\"pcm\",\"opus\"]}");
    return;
  }
  if (type == WStype_BIN) {
    // The loop publishes completion after worker cleanup; late frames belong to
    // that finished playback and must not poison its already-final result.
    if (taskFinished_) return;
    if (!playing()) { fail("invalid_audio"); return; }
    if (request_.codec == Codec::OPUS) {
      const uint32_t callbackStartedUs = micros();
      handleOpusPacket(payload, length);
      const uint32_t callbackUs = micros() - callbackStartedUs;
      ++opusCallbackCount_;
      opusCallbackTotalUs_ += callbackUs;
      if (callbackUs > opusCallbackWorstUs_) opusCallbackWorstUs_ = callbackUs;
      return;
    }
    const uint32_t limitBytes = request_.streaming ? request_.maxBytes : request_.bytes;
    if (!newoValidSpeakerChunk(length, receivedBytes_, limitBytes, endReceived_)) {
      fail("invalid_pcm"); return;
    }
    if (firstPcmReceivedMs_ == 0) firstPcmReceivedMs_ = millis();
    lastPcmReceivedMs_ = millis();
    // Never wait in the WebSocket callback. Receiver-driven credit on the VPS
    // keeps delivered PCM bounded below this fixed 24 KiB StreamBuffer.
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
  const uint32_t received = receivedBytes_;
  const uint32_t consumed = consumedBytes_;
  const bool receiveProgress = received > lastFlowSentReceivedBytes_;
  const bool consumeProgress = consumed > lastFlowSentConsumedBytes_;
  if (!force && !receiveProgress && !consumeProgress) return;

  JsonDocument doc;
  doc["type"] = "speaker_flow";
  doc["playback_id"] = request_.playbackId;
  doc["codec"] = request_.codec == Codec::OPUS ? "opus" : "pcm";
  doc["received_bytes"] = received;
  doc["consumed_bytes"] = consumed;
  doc["buffered_bytes"] = static_cast<uint32_t>(xStreamBufferBytesAvailable(buffer_));
  doc["capacity_bytes"] = static_cast<uint32_t>(NewoConfig::SPEAKER_BUFFER_BYTES);
  String body;
  serializeJson(doc, body);
  webSocket_.sendTXT(body);
  if (receiveProgress) ++receivedFlowReportCount_;
  lastFlowSentReceivedBytes_ = received;
  lastFlowSentConsumedBytes_ = consumed;
  lastFlowReportMs_ = millis();
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

    const uint32_t timeoutMs = request_.streaming
        ? NewoConfig::SPEAKER_STREAM_ABSOLUTE_TIMEOUT_MS
        : 15'000 + (request_.bytes * 1'000UL / NewoConfig::SPEAKER_PCM_BYTES_PER_SECOND);
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
        const bool allPcmReceived = endReceived_ && decoderFinished_ && receivedBytes_ == request_.bytes;
        if (available >= NewoConfig::SPEAKER_PREBUFFER_BYTES ||
            (allPcmReceived && available >= sizeof(int16_t))) {
          playbackStarted = true;
          playbackStarted_ = true;
          playbackStartedMs = millis();
          minimumBufferedBytes_ = static_cast<uint32_t>(available);
          strlcpy(playbackStartedEvent_.playbackId, request_.playbackId,
                  sizeof(playbackStartedEvent_.playbackId));
          playbackStartedEvent_.firstPcmToPlayMs = firstPcmReceivedMs_ == 0
              ? 0 : playbackStartedMs - firstPcmReceivedMs_;
          playbackStartedEventReady_ = true;
          char detail[96];
          snprintf(detail, sizeof(detail), "id=%s codec=%s first_pcm_to_play_ms=%lu", request_.playbackId,
                   request_.codec == Codec::OPUS ? "opus" : "pcm",
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
      } else if (playbackStarted && !endReceived_ && !underrunActive) {
        ++underrunCount_;
        minimumBufferedBytes_ = 0;
        underrunActive = true;
      }
      if (endReceived_ && decoderFinished_ && xStreamBufferBytesAvailable(buffer_) == 0) {
        if (receivedBytes_ != request_.bytes || consumedBytes_ != request_.bytes) fail("truncated");
        break;
      }
      const uint32_t nowMs = millis();
      if (!endReceived_ && nowMs - lastPcmReceivedMs_ >= NewoConfig::SPEAKER_STREAM_NO_PROGRESS_TIMEOUT_MS) {
        fail("speaker_stream_timeout");
        break;
      }
      if (nowMs - startedMs >= timeoutMs) { fail("timeout"); break; }
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    // I2SClass::write() only guarantees that PCM was copied into the TX DMA ring.
    // Arduino-ESP32 3.3.11 uses six 240-frame descriptors. Waiting for a full
    // ring plus one additional TX EOF after the final write guarantees the
    // descriptor containing the last audio samples and the hardware FIFO tail
    // have actually drained before SPEAKER_COMPLETE can be emitted.
    if (!failed_ && playbackStarted && endReceived_ && decoderFinished_ && receivedBytes_ == request_.bytes &&
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
  result_.success = !failed_ && endReceived_ && decoderFinished_ && receivedBytes_ == request_.bytes &&
                    consumedBytes_ == request_.bytes;
  strlcpy(result_.error, result_.success ? "" : (failureReason_ ? failureReason_ : "unknown"), sizeof(result_.error));
  playbackStarted_ = false;
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
    const uint32_t received = receivedBytes_;
    const uint32_t consumed = consumedBytes_;
    const uint32_t buffered = static_cast<uint32_t>(xStreamBufferBytesAvailable(buffer_));
    const bool receiveProgress = received - lastFlowSentReceivedBytes_ >= NewoConfig::SPEAKER_RECEIVE_REPORT_BYTES;
    const bool consumeProgress = consumed - lastFlowSentConsumedBytes_ >= NewoConfig::SPEAKER_CONSUME_REPORT_BYTES;
    const bool lowWaterHeartbeat = playbackStarted_ && buffered < NewoConfig::SPEAKER_LOW_WATER_BYTES &&
        millis() - lastFlowReportMs_ >= NewoConfig::SPEAKER_LOW_WATER_REPORT_INTERVAL_MS;
    const bool finalProgress = endReceived_ && consumed == request_.bytes &&
        (received != lastFlowSentReceivedBytes_ || consumed != lastFlowSentConsumedBytes_);
    if (receiveProgress || consumeProgress || lowWaterHeartbeat || finalProgress) {
      sendFlowReport(lowWaterHeartbeat || finalProgress);
    }
  }

  // Do not expose a result or allow a replacement playback until the decoder
  // task has released its queue/decoder ownership too.
  if (taskFinished_ && !decoderTask_) {
    task_ = nullptr;
    // The decoder task owns/destroys its Opus state; it may still be draining.
    if (playbackStateApplied_) {
      display_.setSpeaking(false);
      audio_.setPlaybackActive(false);
      playbackStateApplied_ = false;
    }
    resultReady_ = true;
    lastPlayback_ = result_.success ? LastPlayback::COMPLETE : LastPlayback::FAILED;
    char diagnostics[144];
    snprintf(diagnostics, sizeof(diagnostics), "codec=%s rx=%lu duration_ms=%lu underruns=%lu min_buffer=%lu drain_ms=%lu stack_low=%lu",
             request_.codec == Codec::OPUS ? "opus" : "pcm",
             static_cast<unsigned long>(result_.bytes), static_cast<unsigned long>(playbackDurationMs_),
             static_cast<unsigned long>(underrunCount_),
             static_cast<unsigned long>(minimumBufferedBytes_ == UINT32_MAX ? 0 : minimumBufferedBytes_),
             static_cast<unsigned long>(i2sDrainMs_),
             static_cast<unsigned long>(minimumTaskStackBytes_));
    NewoLog::log(underrunCount_ == 0 ? NewoLog::Level::INFO : NewoLog::Level::WARN,
                 NewoLog::Subsystem::AUDIO, "SPEAKER_DIAGNOSTICS", diagnostics);
    char bufferDiagnostics[128];
    snprintf(bufferDiagnostics, sizeof(bufferDiagnostics), "overflows=%lu max_buffer=%lu capacity=%u chunk=%u flow_reports=%lu rx_reports=%lu",
             static_cast<unsigned long>(overflowCount_), static_cast<unsigned long>(maximumBufferedBytes_),
             static_cast<unsigned>(NewoConfig::SPEAKER_BUFFER_BYTES),
             static_cast<unsigned>(NewoConfig::SPEAKER_CHUNK_BYTES),
             static_cast<unsigned long>(flowReportCount_),
             static_cast<unsigned long>(receivedFlowReportCount_));
    NewoLog::log(overflowCount_ == 0 ? NewoLog::Level::INFO : NewoLog::Level::ERROR,
                 NewoLog::Subsystem::AUDIO, "SPEAKER_BUFFER", bufferDiagnostics);
    if (request_.codec == Codec::OPUS) {
      char opusDiagnostics[320];
      const uint32_t averageDecodeUs = opusDecodeCount_ == 0 ? 0 :
          static_cast<uint32_t>(opusDecodeTotalUs_ / opusDecodeCount_);
      const uint32_t averageCallbackUs = opusCallbackCount_ == 0 ? 0 :
          static_cast<uint32_t>(opusCallbackTotalUs_ / opusCallbackCount_);
      snprintf(opusDiagnostics, sizeof(opusDiagnostics),
               "packets_rx=%lu decoded=%lu wire_bytes=%lu decoded_pcm=%lu q_high_packets=%lu q_high_bytes=%lu q_overflows=%lu decode_avg_us=%lu decode_worst_us=%lu decoder_errors=%lu decoder_stack_low=%lu callback_avg_us=%lu callback_worst_us=%lu",
               static_cast<unsigned long>(opusPacketsReceived_), static_cast<unsigned long>(opusDecodeCount_),
               static_cast<unsigned long>(opusBytesReceived_), static_cast<unsigned long>(receivedBytes_),
               static_cast<unsigned long>(opusQueueHighWaterPackets_), static_cast<unsigned long>(opusQueueHighWaterBytes_),
               static_cast<unsigned long>(opusQueueOverflows_), static_cast<unsigned long>(averageDecodeUs),
               static_cast<unsigned long>(opusDecodeWorstUs_), static_cast<unsigned long>(opusDecoderErrors_),
               static_cast<unsigned long>(minimumDecoderStackBytes_ == UINT32_MAX ? 0 : minimumDecoderStackBytes_),
               static_cast<unsigned long>(averageCallbackUs), static_cast<unsigned long>(opusCallbackWorstUs_));
      NewoLog::log(opusDecoderErrors_ == 0 ? NewoLog::Level::INFO : NewoLog::Level::ERROR,
                   NewoLog::Subsystem::AUDIO, "SPEAKER_OPUS", opusDiagnostics);
    }
    NewoLog::log(result_.success ? NewoLog::Level::INFO : NewoLog::Level::ERROR,
                 NewoLog::Subsystem::AUDIO, result_.success ? "SPEAKER_COMPLETE" : "SPEAKER_FAILED",
                 result_.success ? "" : result_.error);
    taskFinished_ = false;
    if (releaseRequested_ || (!enabled_ && !temporaryRequested_)) releaseResources();
  }
  if (releaseRequested_ && !playing()) {
    releaseResources();
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
