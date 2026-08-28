#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>

#include "newo_wifi.h"

class NewoCloud {
 public:
  explicit NewoCloud(NewoWiFi& wifi);

  void begin();
  void loop();
  bool connected() const;
  bool consumeVoiceResetRequest();
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
  void sendVoiceResetAck(const char* requestId);

  NewoWiFi& wifi_;
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
  bool voiceResetRequested_ = false;
  uint32_t minimumLoopStackBytes_ = UINT32_MAX;
};
