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

  uint8_t speakerVolume() const { return speakerVolume_; }
  bool speakerMuted() const { return speakerMuted_; }
  bool speakerEnabled() const { return speakerEnabled_; }
  bool clockEnabled() const { return clockEnabled_; }
  bool setSpeakerVolume(uint8_t volume);
  bool setSpeakerMuted(bool muted);
  bool setSpeakerEnabled(bool enabled);
  bool setClockEnabled(bool enabled);

 private:
  bool loadNetworks();
  bool saveNetworks(const std::vector<NewoWifiCredential>& networks);
  bool isCredentialValid(const String& ssid, const String& password) const;

  Preferences preferences_;
  std::vector<NewoWifiCredential> networks_;
  uint8_t speakerVolume_ = 100;
  bool speakerMuted_ = false;
  bool speakerEnabled_ = true;
  bool clockEnabled_ = true;
  bool started_ = false;
};
