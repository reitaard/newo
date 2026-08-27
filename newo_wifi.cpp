#include "newo_wifi.h"

#include <ESPmDNS.h>

#include "newo_config.h"

NewoWiFi::NewoWiFi(NewoStorage& storage) : storage_(storage) {}

void NewoWiFi::reloadSavedNetworks() {
  wifiMulti_.APlistClean();
  wifiMulti_.setStrictMode(true);
  wifiMulti_.setAllowOpenAP(false);

  for (const auto& network : storage_.networks()) {
    if (network.password.length() == 0) {
      wifiMulti_.addAP(network.ssid.c_str());
    } else {
      wifiMulti_.addAP(network.ssid.c_str(), network.password.c_str());
    }
  }
}

void NewoWiFi::begin() {
  WiFi.setAutoReconnect(true);
  reloadSavedNetworks();

  if (storage_.count() == 0) {
    Serial.println("[wifi] No saved networks");
    startSetupAP();
    return;
  }

  WiFi.mode(WIFI_STA);
  Serial.printf("[wifi] Trying %u saved network(s)...\n", static_cast<unsigned>(storage_.count()));

  if (!tryConnect(NewoConfig::INITIAL_CONNECT_TIMEOUT_MS)) {
    Serial.println("[wifi] No saved network is reachable; starting setup AP");
    startSetupAP();
  }
}

bool NewoWiFi::tryConnect(uint32_t timeoutMs) {
  if (storage_.count() == 0) {
    return false;
  }

  const uint8_t status = wifiMulti_.run(timeoutMs);
  lastReconnectAttemptMs_ = millis();

  if (status != WL_CONNECTED) {
    stopMdns();
    return false;
  }

  Serial.printf("[wifi] Connected: %s\n", WiFi.SSID().c_str());
  Serial.printf("[wifi] IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[wifi] RSSI: %d dBm\n", WiFi.RSSI());
  ensureMdns();
  return true;
}

void NewoWiFi::startSetupAP() {
  if (setupApActive_) {
    return;
  }

  WiFi.mode(WIFI_AP_STA);

  if (!WiFi.softAP(NewoConfig::SETUP_AP_SSID)) {
    Serial.println("[wifi] Failed to start setup AP");
    return;
  }

  setupApActive_ = true;
  Serial.printf("[wifi] Setup AP: %s\n", NewoConfig::SETUP_AP_SSID);
  Serial.printf("[wifi] Setup URL: http://%s\n", WiFi.softAPIP().toString().c_str());
}

void NewoWiFi::ensureMdns() {
  if (mdnsStarted_ || !connected()) {
    return;
  }

  if (MDNS.begin(NewoConfig::MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted_ = true;
    Serial.printf("[wifi] Local name: http://%s.local\n", NewoConfig::MDNS_HOST);
  } else {
    Serial.println("[wifi] mDNS failed to start");
  }
}

void NewoWiFi::stopMdns() {
  if (!mdnsStarted_) {
    return;
  }

  MDNS.end();
  mdnsStarted_ = false;
}

void NewoWiFi::loop() {
  if (connected()) {
    ensureMdns();
    return;
  }

  stopMdns();

  if (storage_.count() == 0) {
    startSetupAP();
    return;
  }

  const uint32_t now = millis();
  if (now - lastReconnectAttemptMs_ < NewoConfig::RECONNECT_INTERVAL_MS) {
    return;
  }

  Serial.println("[wifi] Reconnecting...");
  if (!tryConnect(NewoConfig::RECONNECT_TIMEOUT_MS)) {
    startSetupAP();
  }
}

bool NewoWiFi::connected() const {
  return WiFi.status() == WL_CONNECTED;
}

bool NewoWiFi::setupApActive() const {
  return setupApActive_;
}

String NewoWiFi::connectedSsid() const {
  return connected() ? WiFi.SSID() : String();
}

IPAddress NewoWiFi::localIP() const {
  return WiFi.localIP();
}

IPAddress NewoWiFi::setupIP() const {
  return WiFi.softAPIP();
}

int32_t NewoWiFi::rssi() const {
  return connected() ? WiFi.RSSI() : 0;
}
