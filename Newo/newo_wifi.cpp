#include "newo_wifi.h"

#include <ESPmDNS.h>
#include <esp_random.h>

#include "newo_config.h"

namespace {

const char* wifiStatusName(uint8_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

}  // namespace

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
  WiFi.onEvent(
      [](WiFiEvent_t event, WiFiEventInfo_t info) {
        if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
          return;
        }

        const auto reason = static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason);
        Serial.printf("[wifi] STA disconnect reason: %u (%s)\n",
                      static_cast<unsigned>(reason),
                      WiFi.STA.disconnectReasonName(reason));
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

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
    Serial.printf("[wifi] Connection attempt failed: %u (%s)\n",
                  static_cast<unsigned>(status), wifiStatusName(status));
    stopMdns();
    return false;
  }

  Serial.printf("[wifi] Connected: %s\n", WiFi.SSID().c_str());
  Serial.printf("[wifi] IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[wifi] RSSI: %d dBm\n", WiFi.RSSI());
  ensureMdns();
  return true;
}

String NewoWiFi::generateSetupPassword() const {
  static constexpr char alphabet[] =
      "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";

  String password;
  password.reserve(13);

  for (uint8_t i = 0; i < 12; ++i) {
    password += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  }

  return password;
}

void NewoWiFi::startSetupAP() {
  if (setupApActive_) {
    return;
  }

  // Enable Wi-Fi before reading the ESP32-S3 hardware RNG so RF entropy is active.
  // WIFI_AP_STA preserves an existing station connection while adding the setup AP.
  WiFi.mode(WIFI_AP_STA);
  setupApPassword_ = generateSetupPassword();

  if (!WiFi.softAP(NewoConfig::SETUP_AP_SSID, setupApPassword_.c_str())) {
    Serial.println("[wifi] Failed to start setup AP");
    setupApPassword_.clear();
    return;
  }

  setupApActive_ = true;
  setupApExpiresAtMs_ = 0;
  Serial.printf("[wifi] Setup AP: %s\n", NewoConfig::SETUP_AP_SSID);
  Serial.printf("[wifi] Setup password: %s\n", setupApPassword_.c_str());
  Serial.printf("[wifi] Setup URL: http://%s\n", WiFi.softAPIP().toString().c_str());
}

bool NewoWiFi::startTemporarySetupAP(uint32_t timeoutMs) {
  if (timeoutMs == 0) {
    return false;
  }

  if (!setupApActive_) {
    startSetupAP();
  }

  if (!setupApActive_) {
    return false;
  }

  setupApExpiresAtMs_ = millis() + timeoutMs;
  return true;
}

void NewoWiFi::stopSetupAP() {
  if (!setupApActive_) {
    return;
  }

  // Disconnect only the soft AP; leave an existing station connection intact.
  WiFi.softAPdisconnect(true);
  setupApActive_ = false;
  setupApExpiresAtMs_ = 0;
  setupApPassword_.clear();
  Serial.println("[wifi] Temporary setup AP stopped");
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
  if (setupApActive_ && setupApExpiresAtMs_ != 0 &&
      static_cast<int32_t>(millis() - setupApExpiresAtMs_) >= 0) {
    stopSetupAP();
  }

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

String NewoWiFi::setupApPassword() const {
  return setupApPassword_;
}

uint32_t NewoWiFi::setupApRemainingSeconds() const {
  if (!setupApActive_ || setupApExpiresAtMs_ == 0) {
    return 0;
  }

  const int32_t remainingMs = static_cast<int32_t>(setupApExpiresAtMs_ - millis());
  if (remainingMs <= 0) {
    return 0;
  }

  return static_cast<uint32_t>((remainingMs + 999) / 1000);
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
