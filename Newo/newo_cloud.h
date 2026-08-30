#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>

#include "newo_display.h"
#include "newo_voice_state.h"
#include "newo_wifi.h"

class NewoCloud {
 public:
  NewoCloud(NewoWiFi& wifi, NewoDisplay& display);

  void begin();
  void loop();
  bool connected() const;
  struct VoiceRequest { enum class Action : uint8_t { ON, OFF, TOGGLE }; Action action; char requestId[40]; };
  struct SpeakerRequest { char playbackId[40]; uint32_t sampleRate; uint32_t bytes; uint8_t channels; uint8_t bitsPerSample; };
  bool consumeVoiceRequest(VoiceRequest& request);
  bool consumeSpeakerRequest(SpeakerRequest& request);
  void sendSpeakerResult(const char* playbackId, bool success, uint32_t bytes, const char* error = nullptr);
  void sendVoiceAck(const char* requestId, NewoVoiceState state, bool voiceConnected,
                    uint32_t wakes, uint32_t sessions);
  void updateVoiceTelemetry(NewoVoiceState state, bool connected, uint32_t wakes,
                            uint32_t sessions, uint32_t failures, uint32_t timeouts);
  // Temporary physical-validation instrumentation; ESP-IDF reports bytes on ESP32-S3.
  void recordStack(const char* point);

 private:
  void startConnection();
  void handleEvent(WStype_t type, uint8_t* payload, size_t length);
  void handleTextMessage(const uint8_t* payload, size_t length);
  void sendHello();
  void sendStatus(const char* requestId = nullptr, bool pong = false);
  void sendHealth(const char* requestId);
  void sendLogs(const char* requestId, uint8_t limit, const char* minLevel);
  void sendRebootAck(const char* requestId);
  void sendDisplayAck(const char* requestId, const char* mode);

  NewoWiFi& wifi_;
  NewoDisplay& display_;
  WebSocketsClient webSocket_;
  bool configured_ = false;
  bool started_ = false;
  bool connected_ = false;
  uint32_t lastStatusMs_ = 0;
  uint32_t rebootAtMs_ = 0;
  uint32_t connectionCount_ = 0;
  uint32_t disconnectCount_ = 0;
  uint32_t errorCount_ = 0;
  bool authenticated_ = false;
  // Control requests are consumed by the Arduino loop. A small FIFO prevents a
  // pending streaming cancellation from making a later OFF/toggle disappear.
  static constexpr uint8_t kVoiceRequestQueueDepth = 4;
  VoiceRequest voiceRequests_[kVoiceRequestQueueDepth] = {};
  uint8_t voiceRequestHead_ = 0;
  uint8_t voiceRequestTail_ = 0;
  uint8_t voiceRequestCount_ = 0;
  static constexpr uint8_t kSpeakerRequestQueueDepth = 2;
  SpeakerRequest speakerRequests_[kSpeakerRequestQueueDepth] = {};
  uint8_t speakerRequestHead_ = 0;
  uint8_t speakerRequestTail_ = 0;
  uint8_t speakerRequestCount_ = 0;
  NewoVoiceState voiceState_ = NewoVoiceState::OFF;
  bool voiceConnected_ = false;
  uint32_t voiceWakes_ = 0, voiceSessions_ = 0, voiceFailures_ = 0, voiceTimeouts_ = 0;
  uint32_t minimumLoopStackBytes_ = UINT32_MAX;
};
