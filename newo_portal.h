#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "newo_storage.h"
#include "newo_wifi.h"

class NewoPortal {
 public:
  NewoPortal(NewoStorage& storage, NewoWiFi& wifi);

  void begin();
  void loop();

 private:
  void syncDnsState();
  void handleRoot();
  void handleStatus();
  void handleScan();
  void handleSaveNetwork();
  void handleDeleteNetwork();
  void handleClearNetworks();
  void handleNotFound();
  void scheduleReboot();

  String htmlEscape(const String& value) const;
  String buildHomePage();

  NewoStorage& storage_;
  NewoWiFi& wifi_;
  DNSServer dnsServer_;
  WebServer server_{80};
  bool dnsRunning_ = false;
  bool rebootScheduled_ = false;
  uint32_t rebootAtMs_ = 0;
};
