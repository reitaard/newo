#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ESP_I2S.h>

#include "newo_config.h"
#include "newo_wifi.h"
#include "newo_voice_health.h"

class NewoAudio {
 public:
  explicit NewoAudio(NewoWiFi& wifi);

  void begin();
  void loop();
  void resetVoiceStream(const char* reason);

 private:
  struct AudioFrame {
    int16_t samples[NewoConfig::AUDIO_SAMPLES_PER_FRAME];
  };

  static void audioTaskEntry(void* context);
  void audioTask();
  void startVoiceConnection();
  void handleVoiceEvent(WStype_t type, uint8_t* payload, size_t length);
  void logLevel(const AudioFrame& frame);
  uint32_t discardQueuedFrames();
  void evaluateVoiceHealth(uint32_t sendDurationUs = 0);
  static const char* healthReasonName(VoiceHealthReason reason);

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
  uint32_t automaticVoiceResets_ = 0;
  uint32_t reconnectCount_ = 0;
  uint32_t bundleSends_ = 0;
  uint32_t maxSendDurationUsSinceLevel_ = 0;
  UBaseType_t queueHighWaterSinceLevel_ = 0;
  uint32_t lastHealthDropped_ = 0;
  uint32_t lastHealthOverruns_ = 0;
  bool resetReconnectPending_ = false;
  uint32_t clippedSamplesSinceLevel_ = 0;
  uint32_t pcmSamplesSinceLevel_ = 0;
  uint32_t lastLevelLogMs_ = 0;
  int32_t inputWords_[NewoConfig::AUDIO_SAMPLES_PER_FRAME * 2] = {};
  // Member storage keeps the 3.2 KiB message out of the loop task's stack.
  uint8_t bundle_[NewoConfig::AUDIO_WS_BUNDLE_BYTES] = {};
  NewoVoiceHealth voiceHealth_{NewoConfig::VOICE_HEALTH_QUEUE_THRESHOLD,
                               NewoConfig::VOICE_HEALTH_SEND_WARN_US,
                               NewoConfig::VOICE_HEALTH_SEND_FATAL_US,
                               NewoConfig::VOICE_HEALTH_SUSTAIN_MS,
                               NewoConfig::VOICE_HEALTH_RESET_COOLDOWN_MS};
};
