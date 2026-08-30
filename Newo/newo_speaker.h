#pragma once

#include <Arduino.h>
#include <ESP_I2S.h>
#include <WebSocketsClient.h>
#include <freertos/stream_buffer.h>

#include "newo_audio.h"
#include "newo_display.h"
#include "newo_wifi.h"

// Dedicated MAX98357A output. This component never shares the microphone's I2S
// object or changes the user's persistent voice enablement choice.
class NewoSpeaker {
 public:
  struct Request {
    char playbackId[40];
    uint32_t sampleRate;
    uint32_t bytes;
    uint8_t channels;
    uint8_t bitsPerSample;
  };
  struct Result {
    char playbackId[40];
    uint32_t bytes;
    bool success;
    char error[48];
  };

  NewoSpeaker(NewoWiFi& wifi, NewoDisplay& display, NewoAudio& audio);
  void begin();
  void loop();
  bool play(const Request& request);
  bool consumeResult(Result& result);
  bool playing() const { return task_ != nullptr; }

 private:
  static void taskEntry(void* context);
  void playbackTask();
  void handleEvent(WStype_t type, uint8_t* payload, size_t length);
  void fail(const char* error);

  NewoWiFi& wifi_;
  NewoDisplay& display_;
  NewoAudio& audio_;
  I2SClass i2s_;
  WebSocketsClient webSocket_;
  StreamBufferHandle_t buffer_ = nullptr;
  TaskHandle_t task_ = nullptr;
  Request request_ = {};
  Result result_ = {};
  volatile bool connected_ = false;
  volatile bool endReceived_ = false;
  volatile bool failed_ = false;
  volatile bool taskFinished_ = false;
  volatile uint32_t receivedBytes_ = 0;
  const char* volatile failureReason_ = nullptr;
  bool resultReady_ = false;
  bool playbackStateApplied_ = false;
  volatile uint32_t minimumTaskStackBytes_ = UINT32_MAX;

  // Fixed object-owned conversion workspace: no large PCM arrays live on the
  // playback task stack. 512 mono bytes expand to exactly 1,024 stereo bytes.
  static constexpr size_t kMonoWorkingBytes = 512;
  static constexpr size_t kWorkingSamples = kMonoWorkingBytes / sizeof(int16_t);
  int16_t monoWorking_[kWorkingSamples] = {};
  int16_t stereoWorking_[kWorkingSamples * 2] = {};
  static_assert(sizeof(monoWorking_) == 512, "speaker mono workspace changed");
  static_assert(sizeof(stereoWorking_) == 1024, "speaker stereo workspace changed");
};
