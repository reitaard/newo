#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <vector>

struct NewoWifiCredential {
  String ssid;
  String password;
};

class NewoStorage {
 public:
  bool begin();

  const std::vector<NewoWifiCredential>& networks() const;
  size_t count() const;

  bool addOrUpdateNetwork(const String& ssid, const String& password);

 private:
  bool loadNetworks();
  bool saveNetworks(const std::vector<NewoWifiCredential>& networks);
  bool isCredentialValid(const String& ssid, const String& password) const;

  Preferences preferences_;
  std::vector<NewoWifiCredential> networks_;
  bool started_ = false;
};
