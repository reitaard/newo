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
  struct VoiceRequest { enum class Action : uint8_t { ON, OFF, TOGGLE, MANUAL_TOGGLE }; Action action; char requestId[40]; };
  struct SpeakerControlRequest {
    enum class Action : uint8_t { STATUS, SET_VOLUME, TOGGLE_MUTE, SET_ENABLED, TEMPORARY_CONNECT };
    Action action;
    uint8_t volume;
    bool enabled;
    char requestId[40];
  };
  bool consumeVoiceRequest(VoiceRequest& request);
  bool consumeSpeakerControlRequest(SpeakerControlRequest& request);
  void sendSpeakerStarted(const char* playbackId, uint32_t firstPcmToPlayMs);
  void sendSpeakerResult(const char* playbackId, bool success, uint32_t bytes, const char* error = nullptr);
  void sendSpeakerAck(const char* requestId, bool enabled, const char* connection, uint8_t volume,
                      bool muted, bool applied, const char* lastPlayback,
                      uint32_t underruns, uint32_t overflows);
  void sendVoiceAck(const char* requestId, NewoVoiceState state, bool voiceConnected,
                    uint32_t wakes, uint32_t sessions, uint32_t failures, uint32_t timeouts,
                    bool applied = true);
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
  static constexpr uint8_t kSpeakerControlQueueDepth = 4;
  SpeakerControlRequest speakerControlRequests_[kSpeakerControlQueueDepth] = {};
  uint8_t speakerControlRequestHead_ = 0;
  uint8_t speakerControlRequestTail_ = 0;
  uint8_t speakerControlRequestCount_ = 0;
  NewoVoiceState voiceState_ = NewoVoiceState::OFF;
  bool voiceConnected_ = false;
  uint32_t voiceWakes_ = 0, voiceSessions_ = 0, voiceFailures_ = 0, voiceTimeouts_ = 0;
  uint32_t minimumLoopStackBytes_ = UINT32_MAX;
};
