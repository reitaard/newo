#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "newo_storage.h"
#include "newo_wifi.h"

class NewoPortal {
 public:
  NewoPortal(NewoStorage& storage, NewoWiFi& wifi) : storage_(storage), wifi_(wifi) {}
  void begin();
  void loop();

 private:
  void handleRoot();
  void handleSave();
  void handleCaptiveProbe();
  String buildPage();
  static String escape(const String& value);

  NewoStorage& storage_;
  NewoWiFi& wifi_;
  DNSServer dns_;
  WebServer server_{80};
  bool servicesStarted_ = false;
  uint32_t rebootAtMs_ = 0;
};
