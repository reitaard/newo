#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ESP_I2S.h>

#include "newo_config.h"
#include "newo_wifi.h"

class NewoAudio {
 public:
  explicit NewoAudio(NewoWiFi& wifi);

  void begin();
  void loop();

 private:
  struct AudioFrame {
    int16_t samples[NewoConfig::AUDIO_SAMPLES_PER_FRAME];
  };

  static void audioTaskEntry(void* context);
  void audioTask();
  void startVoiceConnection();
  void handleVoiceEvent(WStype_t type, uint8_t* payload, size_t length);
  void logLevel(const AudioFrame& frame);
  void discardQueuedFrames();

  NewoWiFi& wifi_;
  I2SClass i2s_;
  WebSocketsClient voiceWebSocket_;
  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  bool configured_ = false;
  bool started_ = false;
  volatile bool voiceConnected_ = false;
  uint32_t capturedFrames_ = 0;
  uint32_t transmittedFrames_ = 0;
  uint32_t droppedFrames_ = 0;
  uint32_t queueOverruns_ = 0;
  uint32_t reconnectCount_ = 0;
  uint32_t clippedSamplesSinceLevel_ = 0;
  uint32_t pcmSamplesSinceLevel_ = 0;
  uint32_t lastLevelLogMs_ = 0;
  int32_t inputWords_[NewoConfig::AUDIO_SAMPLES_PER_FRAME * 2] = {};
};
