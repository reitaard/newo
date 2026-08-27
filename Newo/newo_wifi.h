#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>

#include "newo_storage.h"

class NewoWiFi {
 public:
  explicit NewoWiFi(NewoStorage& storage);

  void begin();
  void loop();
  void reloadSavedNetworks();

  bool connected() const;
  bool setupApActive() const;
  String setupApPassword() const;
  String connectedSsid() const;
  IPAddress localIP() const;
  IPAddress setupIP() const;
  int32_t rssi() const;

 private:
  bool tryConnect(uint32_t timeoutMs);
  void startSetupAP();
  String generateSetupPassword() const;
  void ensureMdns();
  void stopMdns();

  NewoStorage& storage_;
  WiFiMulti wifiMulti_;
  String setupApPassword_;
  bool setupApActive_ = false;
  bool mdnsStarted_ = false;
  uint32_t lastReconnectAttemptMs_ = 0;
};
