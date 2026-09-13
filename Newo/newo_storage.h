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
  bool usbHostEnabled() const { return usbHostEnabled_; }
  bool usbAudioEnabled() const { return usbAudioEnabled_; }
  bool usbStorageEnabled() const { return usbStorageEnabled_; }
  bool usbVcpEnabled() const { return usbVcpEnabled_; }
  bool usbTrialPending() const { return usbTrialPending_; }
  uint8_t micProcessingMode() const { return micProcessingMode_; }
  uint8_t micNsLevel() const { return micNsLevel_; }
  bool setSpeakerVolume(uint8_t volume);
  bool setSpeakerMuted(bool muted);
  bool setSpeakerEnabled(bool enabled);
  bool setClockEnabled(bool enabled);
  bool setUsbHostEnabled(bool enabled);
  bool setUsbAudioEnabled(bool enabled);
  bool setUsbStorageEnabled(bool enabled);
  bool setUsbVcpEnabled(bool enabled);
  bool setUsbTrialPending(bool pending);
  bool setMicProcessing(uint8_t mode, uint8_t nsLevel);

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
  bool usbHostEnabled_ = false;
  bool usbAudioEnabled_ = true;
  bool usbStorageEnabled_ = false;
  bool usbVcpEnabled_ = false;
  bool usbTrialPending_ = false;
  uint8_t micProcessingMode_ = 1;
  uint8_t micNsLevel_ = 1;
  bool started_ = false;
};
