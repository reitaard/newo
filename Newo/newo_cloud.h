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

 private:
  void startConnection();
  void handleEvent(WStype_t type, uint8_t* payload, size_t length);
  void handleTextMessage(const uint8_t* payload, size_t length);
  void sendHello();
  void sendStatus(const char* requestId = nullptr, bool pong = false);

  NewoWiFi& wifi_;
  WebSocketsClient webSocket_;
  bool configured_ = false;
  bool started_ = false;
  bool connected_ = false;
  uint32_t lastStatusMs_ = 0;
};
