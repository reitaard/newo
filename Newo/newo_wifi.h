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
  bool startTemporarySetupAP(uint32_t timeoutMs);

  bool connected() const;
  bool setupApActive() const;
  String setupApPassword() const;
  uint32_t setupApRemainingSeconds() const;
  String connectedSsid() const;
  IPAddress localIP() const;
  IPAddress setupIP() const;
  int32_t rssi() const;

 private:
  bool tryConnect(uint32_t timeoutMs);
  void startSetupAP();
  void stopSetupAP();
  String generateSetupPassword() const;
  void ensureMdns();
  void stopMdns();

  NewoStorage& storage_;
  WiFiMulti wifiMulti_;
  String setupApPassword_;
  bool setupApActive_ = false;
  uint32_t setupApExpiresAtMs_ = 0;
  bool mdnsStarted_ = false;
  uint32_t lastReconnectAttemptMs_ = 0;
};
