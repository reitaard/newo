#include "newo_speaker.h"

#include <cstring>

#include "newo_config.h"
#include "newo_log.h"

#if __has_include("newo_secrets.h")
#include "newo_secrets.h"
#define NEWO_SPEAKER_HAS_LOCAL_SECRETS 1
#else
#define NEWO_SPEAKER_HAS_LOCAL_SECRETS 0
#endif

NewoSpeaker::NewoSpeaker(NewoWiFi& wifi, NewoDisplay& display, NewoAudio& audio)
    : wifi_(wifi), display_(display), audio_(audio) {}

void NewoSpeaker::begin() {
  buffer_ = xStreamBufferCreate(NewoConfig::SPEAKER_BUFFER_BYTES, 1);
  if (!buffer_) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::AUDIO, "SPEAKER_BUFFER_FAILED");
    return;
  }
  webSocket_.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
    handleEvent(type, payload, length);
  });
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::AUDIO, "SPEAKER_READY", "volume=12.5%");
}

bool NewoSpeaker::play(const Request& request) {
  if (!buffer_ || task_ || resultReady_) return false;
  if (!wifi_.connected() || request.sampleRate != NewoConfig::SPEAKER_SAMPLE_RATE ||
      request.channels != 1 || request.bitsPerSample != 16 || request.bytes == 0 ||
      request.bytes > NewoConfig::SPEAKER_MAX_STREAM_BYTES || (request.bytes & 1)) return false;
  if (!audio_.setPlaybackActive(true)) return false;

  request_ = request;
  result_ = {};
  strlcpy(result_.playbackId, request.playbackId, sizeof(result_.playbackId));
  xStreamBufferReset(buffer_);
  connected_ = false;
  endReceived_ = false;
  failed_ = false;
  taskFinished_ = false;
  receivedBytes_ = 0;
  failureReason_ = nullptr;
  minimumTaskStackBytes_ = UINT32_MAX;
  playbackStateApplied_ = true;
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

void NewoSpeaker::fail(const char* error) {
  if (!failed_) failureReason_ = error;
  failed_ = true;
}

void NewoSpeaker::handleEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    connected_ = true;
    return;
  }
  if (type == WStype_BIN) {
    if (!length || (length & 1) || receivedBytes_ > request_.bytes ||
        length > request_.bytes - receivedBytes_) { fail("invalid_pcm"); return; }
    // Never wait in the WebSocket callback. The VPS paces 2 KiB chunks and this
    // fixed stream buffer absorbs up to 256 ms; overflow fails instead of growing.
    if (xStreamBufferSend(buffer_, payload, length, 0) != length) { fail("buffer_overflow"); return; }
    receivedBytes_ += length;
    return;
  }
  if (type == WStype_TEXT && length) {
    static constexpr char kEnd[] = "\"type\":\"speaker_end\"";
    for (size_t i = 0; i + sizeof(kEnd) - 1 <= length; ++i) {
      if (memcmp(payload + i, kEnd, sizeof(kEnd) - 1) == 0) { endReceived_ = true; break; }
    }
    return;
  }
  if (type == WStype_DISCONNECTED) {
    connected_ = false;
    if (!endReceived_) fail("disconnected");
  } else if (type == WStype_ERROR) {
    fail("socket_error");
  }
}

void NewoSpeaker::playbackTask() {
  minimumTaskStackBytes_ = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
#if !NEWO_SPEAKER_HAS_LOCAL_SECRETS
  fail("secrets_missing");
#else
  i2s_.setPins(NewoConfig::SPEAKER_I2S_BCLK_PIN, NewoConfig::SPEAKER_I2S_WS_PIN,
               NewoConfig::SPEAKER_I2S_DOUT_PIN);
  if (!i2s_.begin(I2S_MODE_STD, NewoConfig::SPEAKER_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                  I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    fail("i2s_failed");
  } else {
    String headers;
    headers.reserve(strlen(NewoSecrets::DEVICE_ID) + strlen(NewoSecrets::DEVICE_SECRET) +
                    strlen(request_.playbackId) + 96);
    headers += F("X-Newo-Device-Id: "); headers += NewoSecrets::DEVICE_ID;
    headers += F("\r\nAuthorization: Bearer "); headers += NewoSecrets::DEVICE_SECRET;
    headers += F("\r\nX-Newo-Playback-Id: "); headers += request_.playbackId;
    webSocket_.setExtraHeaders(headers.c_str());
    webSocket_.beginSslWithCA(NewoConfig::CLOUD_HOST, NewoConfig::CLOUD_PORT,
                              NewoConfig::SPEAKER_PATH, NewoSecrets::CLOUD_CA_CERT, "");

    const uint32_t timeoutMs = 15'000 + (request_.bytes * 1'000UL / NewoConfig::SPEAKER_PCM_BYTES_PER_SECOND);
    const uint32_t startedMs = millis();
    while (!failed_) {
      webSocket_.loop();
      const uint32_t stackBytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
      if (stackBytes < minimumTaskStackBytes_) minimumTaskStackBytes_ = stackBytes;
      size_t available = xStreamBufferBytesAvailable(buffer_);
      if (available >= sizeof(int16_t)) {
        const size_t evenAvailable = available & ~static_cast<size_t>(1);
        const size_t wanted = min(evenAvailable, sizeof(monoWorking_));
        const size_t count = xStreamBufferReceive(buffer_, monoWorking_, wanted, 0);
        if (count & 1) { fail("unaligned_pcm"); continue; }
        const size_t samples = count / sizeof(int16_t);
        if (samples > kWorkingSamples) { fail("working_overflow"); continue; }
        for (size_t i = 0; i < samples; ++i) {
          // Begin at one eighth full-scale for the first MAX98357A test.
          const int16_t sample = monoWorking_[i] / NewoConfig::SPEAKER_DIGITAL_DIVISOR;
          stereoWorking_[i * 2] = sample;
          stereoWorking_[i * 2 + 1] = sample;
        }
        const size_t stereoBytes = samples * 2 * sizeof(int16_t);
        if (stereoBytes > sizeof(stereoWorking_) ||
            i2s_.write(stereoWorking_, stereoBytes) != stereoBytes) {
          fail("i2s_write_failed");
        }
      }
      if (endReceived_ && xStreamBufferBytesAvailable(buffer_) == 0) {
        if (receivedBytes_ != request_.bytes) fail("truncated");
        break;
      }
      if (millis() - startedMs >= timeoutMs) { fail("timeout"); break; }
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    webSocket_.disconnect();
    i2s_.end();
  }
#endif
  const uint32_t finalStackBytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  if (finalStackBytes < minimumTaskStackBytes_) minimumTaskStackBytes_ = finalStackBytes;
  result_.bytes = receivedBytes_;
  result_.success = !failed_ && endReceived_ && receivedBytes_ == request_.bytes;
  strlcpy(result_.error, result_.success ? "" : (failureReason_ ? failureReason_ : "unknown"), sizeof(result_.error));
  taskFinished_ = true;
  vTaskDelete(nullptr);
}

void NewoSpeaker::loop() {
  if (!task_ || !taskFinished_) return;
  task_ = nullptr;
  if (playbackStateApplied_) {
    display_.setSpeaking(false);
    audio_.setPlaybackActive(false);
    playbackStateApplied_ = false;
  }
  resultReady_ = true;
  char detail[80];
  snprintf(detail, sizeof(detail), "bytes=%lu result=%s stack_low=%lu",
           static_cast<unsigned long>(result_.bytes), result_.success ? "complete" : result_.error,
           static_cast<unsigned long>(minimumTaskStackBytes_));
  NewoLog::log(result_.success ? NewoLog::Level::INFO : NewoLog::Level::ERROR,
               NewoLog::Subsystem::AUDIO, result_.success ? "SPEAKER_COMPLETE" : "SPEAKER_FAILED", detail);
}

bool NewoSpeaker::consumeResult(Result& result) {
  if (!resultReady_) return false;
  result = result_;
  resultReady_ = false;
  return true;
}
