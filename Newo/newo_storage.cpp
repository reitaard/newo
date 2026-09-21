#include "newo_storage.h"
#include "newo_console_redirect.h"

#include <ArduinoJson.h>

#include <utility>

#include "newo_config.h"

namespace {
constexpr char kNamespace[] = "newo-wifi";
constexpr char kNetworksKey[] = "networks";
constexpr char kSpeakerVolumeKey[] = "speaker-vol";
constexpr char kSpeakerMutedKey[] = "speaker-mute";
constexpr char kSpeakerEnabledKey[] = "speaker-on";
constexpr char kClockEnabledKey[] = "clock-on";
constexpr char kAlarmVolumeKey[] = "alarm-vol";
constexpr char kUsbHostEnabledKey[] = "usb-host-on";
constexpr char kUsbAudioEnabledKey[] = "usb-audio-on";
constexpr char kUsbStorageEnabledKey[] = "usb-store-on";
constexpr char kUsbVcpEnabledKey[] = "usb-vcp-on";
constexpr char kUsbTrialPendingKey[] = "usb-trial";
constexpr char kMicProcessingModeKey[] = "mic-mode";
constexpr char kMicNsLevelKey[] = "mic-ns-level";
}

bool NewoStorage::begin() {
  if (started_) {
    return true;
  }

  if (!preferences_.begin(kNamespace, false)) {
    Serial.println("[storage] Failed to open NVS namespace");
    return false;
  }

  started_ = true;
  speakerVolume_ = preferences_.getUChar(kSpeakerVolumeKey, 100);
  if (speakerVolume_ > 100) speakerVolume_ = 100;
  speakerMuted_ = preferences_.getBool(kSpeakerMutedKey, false);
  speakerEnabled_ = preferences_.getBool(kSpeakerEnabledKey, true);
  clockEnabled_ = preferences_.getBool(kClockEnabledKey, true);
  alarmVolume_ = preferences_.getUChar(kAlarmVolumeKey, 80);
  if (alarmVolume_ > 100) alarmVolume_ = 80;
  usbHostEnabled_ = preferences_.getBool(kUsbHostEnabledKey, NewoConfig::USB_HOST_DEFAULT_ENABLED);
  usbAudioEnabled_ = preferences_.getBool(kUsbAudioEnabledKey, true);
  usbStorageEnabled_ = preferences_.getBool(kUsbStorageEnabledKey, false);
  usbVcpEnabled_ = preferences_.getBool(kUsbVcpEnabledKey, false);
  usbTrialPending_ = preferences_.getBool(kUsbTrialPendingKey, false);
  micProcessingMode_ = preferences_.getUChar(kMicProcessingModeKey, 1) <= 1
      ? preferences_.getUChar(kMicProcessingModeKey, 1) : 1;
  micNsLevel_ = preferences_.getUChar(kMicNsLevelKey, 1) <= 2
      ? preferences_.getUChar(kMicNsLevelKey, 1) : 1;
  return loadNetworks();
}

const std::vector<NewoWifiCredential>& NewoStorage::networks() const {
  return networks_;
}

size_t NewoStorage::count() const {
  return networks_.size();
}

bool NewoStorage::isCredentialValid(const String& ssid, const String& password) const {
  // IEEE 802.11 and ESP-IDF permit SSIDs up to 32 bytes.
  if (ssid.length() == 0 || ssid.length() > 32) {
    return false;
  }

  // Blank password means an open network. WPA/WPA2 passphrases are 8-63 chars.
  if (password.length() != 0 && (password.length() < 8 || password.length() > 63)) {
    return false;
  }

  return true;
}

bool NewoStorage::loadNetworks() {
  networks_.clear();

  const String raw = preferences_.getString(kNetworksKey, "[]");
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, raw);

  if (error || !doc.is<JsonArray>()) {
    Serial.printf("[storage] Invalid saved network data: %s\n",
                  error ? error.c_str() : "not an array");
    return saveNetworks(networks_);
  }

  for (JsonObject item : doc.as<JsonArray>()) {
    const String ssid = item["ssid"] | "";
    const String password = item["password"] | "";

    if (!isCredentialValid(ssid, password)) {
      continue;
    }

    if (networks_.size() >= NewoConfig::MAX_SAVED_NETWORKS) {
      break;
    }

    networks_.push_back({ssid, password});
  }

  Serial.printf("[storage] Loaded %u saved network(s)\n",
                static_cast<unsigned>(networks_.size()));
  return true;
}

bool NewoStorage::saveNetworks(const std::vector<NewoWifiCredential>& networks) {
  if (!started_) {
    return false;
  }

  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();

  for (const auto& network : networks) {
    JsonObject item = array.add<JsonObject>();
    item["ssid"] = network.ssid;
    item["password"] = network.password;
  }

  String raw;
  serializeJson(doc, raw);

  const size_t written = preferences_.putString(kNetworksKey, raw);
  if (written == 0) {
    Serial.println("[storage] Failed to save network data");
    return false;
  }

  return true;
}

bool NewoStorage::setSpeakerVolume(uint8_t volume) {
  if (!started_ || volume > 100) return false;
  if (volume == speakerVolume_) return true;
  if (preferences_.putUChar(kSpeakerVolumeKey, volume) != sizeof(volume)) return false;
  speakerVolume_ = volume;
  return true;
}

bool NewoStorage::setSpeakerMuted(bool muted) {
  if (!started_ || muted == speakerMuted_) return started_;
  if (preferences_.putBool(kSpeakerMutedKey, muted) != sizeof(muted)) return false;
  speakerMuted_ = muted;
  return true;
}

bool NewoStorage::setSpeakerEnabled(bool enabled) {
  if (!started_ || enabled == speakerEnabled_) return started_;
  if (preferences_.putBool(kSpeakerEnabledKey, enabled) != sizeof(enabled)) return false;
  speakerEnabled_ = enabled;
  return true;
}

bool NewoStorage::setClockEnabled(bool enabled) {
  if (!started_ || enabled == clockEnabled_) return started_;
  if (preferences_.putBool(kClockEnabledKey, enabled) != sizeof(enabled)) return false;
  clockEnabled_ = enabled;
  return true;
}

bool NewoStorage::setAlarmVolume(uint8_t volume) {
  if (!started_ || volume > 100) return false;
  if (volume == alarmVolume_) return true;
  if (preferences_.putUChar(kAlarmVolumeKey, volume) != sizeof(volume)) return false;
  alarmVolume_ = volume;
  return true;
}

bool NewoStorage::setUsbHostEnabled(bool enabled) {
  if (!started_ || enabled == usbHostEnabled_) return started_;
  if (preferences_.putBool(kUsbHostEnabledKey, enabled) != sizeof(enabled)) return false;
  usbHostEnabled_ = enabled;
  return true;
}

#define NEWO_USB_BOOL_SETTER(Name, Key, Field) \
bool NewoStorage::Name(bool enabled) { \
  if (!started_ || enabled == Field) return started_; \
  if (preferences_.putBool(Key, enabled) != sizeof(enabled)) return false; \
  Field = enabled; return true; \
}
NEWO_USB_BOOL_SETTER(setUsbAudioEnabled, kUsbAudioEnabledKey, usbAudioEnabled_)
NEWO_USB_BOOL_SETTER(setUsbStorageEnabled, kUsbStorageEnabledKey, usbStorageEnabled_)
NEWO_USB_BOOL_SETTER(setUsbVcpEnabled, kUsbVcpEnabledKey, usbVcpEnabled_)
NEWO_USB_BOOL_SETTER(setUsbTrialPending, kUsbTrialPendingKey, usbTrialPending_)
#undef NEWO_USB_BOOL_SETTER

bool NewoStorage::setMicProcessing(uint8_t mode, uint8_t nsLevel) {
  if (!started_ || mode > 1 || nsLevel > 2) return false;
  if (mode != micProcessingMode_ && preferences_.putUChar(kMicProcessingModeKey, mode) != sizeof(mode)) return false;
  if (nsLevel != micNsLevel_ && preferences_.putUChar(kMicNsLevelKey, nsLevel) != sizeof(nsLevel)) return false;
  micProcessingMode_ = mode;
  micNsLevel_ = nsLevel;
  return true;
}

bool NewoStorage::addOrUpdateNetwork(const String& ssid, const String& password) {
  if (!started_ || !isCredentialValid(ssid, password)) {
    return false;
  }

  std::vector<NewoWifiCredential> updated = networks_;
  bool found = false;
  for (auto& network : updated) {
    if (network.ssid == ssid) {
      network.password = password;
      found = true;
      break;
    }
  }

  if (!found) {
    if (updated.size() >= NewoConfig::MAX_SAVED_NETWORKS) {
      Serial.println("[storage] Saved network limit reached");
      return false;
    }
    updated.push_back({ssid, password});
  }

  if (!saveNetworks(updated)) {
    return false;
  }

  networks_ = std::move(updated);
  return true;
}
