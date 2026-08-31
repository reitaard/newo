#pragma once

#include <Arduino.h>
#include <ESP_I2S.h>
#include <WebSocketsClient.h>
#include <freertos/stream_buffer.h>
#include <freertos/queue.h>
#include <opus.h>

#include "newo_audio.h"
#include "newo_display.h"
#include "newo_storage.h"
#include "newo_wifi.h"

// Dedicated MAX98357A output. The authenticated /speaker WebSocket is persistent
// only while Speaker is enabled; I2S remains playback-scoped. PCM remains the
// compatibility transport; Opus is decoded into the same bounded PCM buffer.
class NewoSpeaker {
 public:
  struct Result {
    char playbackId[40];
    uint32_t bytes;
    bool success;
    char error[48];
  };
  struct PlaybackStarted {
    char playbackId[40];
    uint32_t firstPcmToPlayMs;
  };

  NewoSpeaker(NewoWiFi& wifi, NewoDisplay& display, NewoAudio& audio, NewoStorage& storage);
  void begin();
  void loop(bool cloudReady);
  bool consumeResult(Result& result);
  bool consumePlaybackStarted(PlaybackStarted& started);

  bool playing() const { return task_ != nullptr || decoderTask_ != nullptr; }
  // True only from the prebuffer/I2S playback boundary through final drain.
  bool audiblePlaybackActive() const { return playbackStarted_; }
  bool enabled() const { return enabled_; }
  bool ready() const { return connected_; }
  bool released() const { return !started_ && !connected_ && !buffer_ && !playing(); }
  const char* connectionStatus() const;
  bool setEnabled(bool enabled);
  bool requestTemporaryConnection();

  uint8_t volume() const { return volume_; }
  bool muted() const { return muted_; }
  bool setVolume(uint8_t volume);
  bool setMuted(bool muted);
  const char* lastPlayback() const;
  uint32_t lastUnderruns() const { return underrunCount_; }
  uint32_t lastOverflows() const { return overflowCount_; }

 private:
  enum class Codec : uint8_t { PCM, OPUS };

  struct Request {
    char playbackId[40];
    uint32_t sampleRate;
    uint32_t bytes;
    uint32_t maxBytes;
    uint16_t opusFrameMs;
    uint16_t opusFramePcmBytes;
    uint8_t channels;
    uint8_t bitsPerSample;
    Codec codec = Codec::PCM;
    bool streaming;
  };
  struct MemorySnapshot {
    uint32_t heap = 0;
    uint32_t minimumHeap = 0;
    uint32_t psram = 0;
  };

  static void taskEntry(void* context);
  static void decoderTaskEntry(void* context);
  static bool IRAM_ATTR onI2sSent(i2s_chan_handle_t handle, i2s_event_data_t* event, void* userData);
  void playbackTask();
  void opusDecoderTask();
  void handleEvent(WStype_t type, uint8_t* payload, size_t length);
  void handleText(const uint8_t* payload, size_t length);
  bool handleOpusPacket(const uint8_t* payload, size_t length);
  bool startPlayback(const Request& request);
  bool allocateBuffer();
  void startConnection();
  void stopConnection(const char* reason);
  void releaseResources();
  void releaseOpusDecoder();
  bool allocateOpusQueue();
  bool resetOpusQueue();
  void releaseOpusQueue();
  void sendFlowReport(bool force = false);
  void sendPlaybackStartedDirect();
  void logMemory(const char* stage, const MemorySnapshot& snapshot, const MemorySnapshot* comparison = nullptr);
  MemorySnapshot memorySnapshot() const;
  void fail(const char* error);
  void publishStartupFailure(const Request& request, const char* error);

  NewoWiFi& wifi_;
  NewoDisplay& display_;
  NewoAudio& audio_;
  NewoStorage& storage_;
  I2SClass i2s_;
  WebSocketsClient webSocket_;
  StreamBufferHandle_t buffer_ = nullptr;
  TaskHandle_t task_ = nullptr;
  TaskHandle_t decoderTask_ = nullptr;
  struct OpusPacketRef { uint8_t slot; uint16_t length; uint16_t sequence; uint16_t validPcmBytes; };
  QueueHandle_t opusReadyQueue_ = nullptr;
  QueueHandle_t opusFreeQueue_ = nullptr;
  uint8_t* opusPacketStorage_ = nullptr;
  Request request_ = {};
  Result result_ = {};
  volatile bool connected_ = false;
  bool started_ = false;
  bool cloudReady_ = false;
  bool enabled_ = true;
  bool temporaryRequested_ = false;
  bool releaseRequested_ = false;
  uint32_t reconnectAfterMs_ = 0;
  MemorySnapshot beforeConnection_ = {};
  MemorySnapshot connectedMemory_ = {};
  bool memoryCycleActive_ = false;
  portMUX_TYPE stateMux_ = portMUX_INITIALIZER_UNLOCKED;

  volatile bool endReceived_ = false;
  volatile bool failed_ = false;
  volatile bool taskFinished_ = false;
  volatile uint32_t receivedBytes_ = 0;
  volatile uint32_t consumedBytes_ = 0;
  volatile uint32_t firstPcmReceivedMs_ = 0;
  volatile uint32_t lastPcmReceivedMs_ = 0;
  const char* volatile failureReason_ = nullptr;
  bool resultReady_ = false;
  volatile bool playbackStartedEventReady_ = false;
  bool playbackStartedDirectSent_ = false;
  volatile bool playbackStarted_ = false;
  PlaybackStarted playbackStartedEvent_ = {};
  bool playbackStateApplied_ = false;
  bool displaySpeakerActiveApplied_ = false;
  volatile uint32_t minimumTaskStackBytes_ = UINT32_MAX;
  volatile uint32_t i2sSentEventCount_ = 0;
  uint32_t lastFlowSentReceivedBytes_ = 0;
  uint32_t lastFlowSentConsumedBytes_ = 0;
  uint32_t lastFlowReportMs_ = 0;
  uint32_t flowReportCount_ = 0;
  uint32_t receivedFlowReportCount_ = 0;
  uint32_t i2sDrainMs_ = 0;
  uint32_t underrunCount_ = 0;
  uint32_t overflowCount_ = 0;
  uint32_t minimumBufferedBytes_ = UINT32_MAX;
  uint32_t maximumBufferedBytes_ = 0;
  uint32_t playbackDurationMs_ = 0;

  OpusDecoder* opusDecoder_ = nullptr;
  volatile bool decoderFinished_ = true;
  volatile bool decoderAbort_ = false;
  volatile uint16_t expectedOpusSequence_ = 0;
  volatile uint32_t opusAdmittedBytes_ = 0;
  uint16_t expectedOpusDecodeSequence_ = 0;
  bool opusSawPartialFrame_ = false;
  uint32_t opusPacketsReceived_ = 0;
  uint32_t opusBytesReceived_ = 0;
  uint32_t opusDecodeCount_ = 0;
  uint32_t opusDecoderErrors_ = 0;
  uint64_t opusDecodeTotalUs_ = 0;
  uint32_t opusDecodeWorstUs_ = 0;
  uint32_t opusQueueHighWaterPackets_ = 0;
  uint32_t opusQueueHighWaterBytes_ = 0;
  uint32_t opusQueueOverflows_ = 0;
  volatile uint32_t opusQueuedWireBytes_ = 0;
  uint32_t opusCallbackCount_ = 0;
  uint64_t opusCallbackTotalUs_ = 0;
  uint32_t opusCallbackWorstUs_ = 0;
  volatile uint32_t minimumDecoderStackBytes_ = UINT32_MAX;

  volatile uint8_t volume_ = 100;
  volatile bool muted_ = false;
  enum class LastPlayback : uint8_t { NONE, COMPLETE, FAILED };
  LastPlayback lastPlayback_ = LastPlayback::NONE;

  // Fixed object-owned conversion workspace: no large PCM arrays live on the
  // playback task stack. 512 mono bytes expand to exactly 1,024 stereo bytes.
  static constexpr size_t kMonoWorkingBytes = 512;
  static constexpr size_t kWorkingSamples = kMonoWorkingBytes / sizeof(int16_t);
  int16_t monoWorking_[kWorkingSamples] = {};
  int16_t stereoWorking_[kWorkingSamples * 2] = {};
  int16_t opusDecoded_[960] = {};  // 40 ms at 24 kHz mono.
  static_assert(sizeof(monoWorking_) == 512, "speaker mono workspace changed");
  static_assert(sizeof(stereoWorking_) == 1024, "speaker stereo workspace changed");
  static_assert(sizeof(opusDecoded_) == 1920, "speaker Opus workspace changed");
};
