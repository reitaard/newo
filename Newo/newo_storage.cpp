#include "newo_storage.h"

#include <ArduinoJson.h>

#include "newo_config.h"

namespace {
constexpr char kNamespace[] = "newo-wifi";
constexpr char kNetworksKey[] = "networks";
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
  return loadNetworks();
}

const std::vector<NewoWifiCredential>& NewoStorage::networks() const {
  return networks_;
}

size_t NewoStorage::count() const {
  return networks_.size();
}

bool NewoStorage::isCredentialValid(const String& ssid, const String& password) const {
  // WiFiMulti in Arduino-ESP32 currently accepts SSIDs up to 31 characters.
  if (ssid.length() == 0 || ssid.length() > 31) {
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
    Serial.printf("[storage] Invalid saved network data: %s\n", error ? error.c_str() : "not an array");
    return saveNetworks();
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

  Serial.printf("[storage] Loaded %u saved network(s)\n", static_cast<unsigned>(networks_.size()));
  return true;
}

bool NewoStorage::saveNetworks() {
  if (!started_) {
    return false;
  }

  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();

  for (const auto& network : networks_) {
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

bool NewoStorage::addOrUpdateNetwork(const String& ssid, const String& password) {
  if (!started_ || !isCredentialValid(ssid, password)) {
    return false;
  }

  for (auto& network : networks_) {
    if (network.ssid == ssid) {
      network.password = password;
      return saveNetworks();
    }
  }

  if (networks_.size() >= NewoConfig::MAX_SAVED_NETWORKS) {
    return false;
  }

  networks_.push_back({ssid, password});
  return saveNetworks();
}

bool NewoStorage::removeNetwork(const String& ssid) {
  if (!started_) {
    return false;
  }

  for (auto it = networks_.begin(); it != networks_.end(); ++it) {
    if (it->ssid == ssid) {
      networks_.erase(it);
      return saveNetworks();
    }
  }

  return false;
}

bool NewoStorage::clearNetworks() {
  if (!started_) {
    return false;
  }

  networks_.clear();
  return saveNetworks();
}
