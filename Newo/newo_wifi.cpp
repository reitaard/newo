#include "newo_wifi.h"
#include "newo_console_redirect.h"

#include <algorithm>
#include <cstring>
#include <esp_wifi.h>

#include "newo_config.h"
#include "newo_log.h"

namespace {

const char* wifiStatusName(wl_status_t status) {
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

void NewoWiFi::begin() {
  WiFi.onEvent([this](arduino_event_id_t eventId, arduino_event_info_t info) {
    handleWiFiEvent(eventId, info);
  });
  WiFi.setAutoReconnect(false);
  // Credentials are owned by NewoStorage; repeated recovery attempts must not
  // rewrite Arduino Wi-Fi's separate system-NVS entry.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  if (storage_.count() == 0) {
    if (recoverLegacyNetwork()) return;
    Serial.println("[wifi] No saved networks; opening browser provisioning");
    startProvisioning();
    return;
  }

  char detail[48];
  snprintf(detail, sizeof(detail), "saved_networks=%u", static_cast<unsigned>(storage_.count()));
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::WIFI, "WIFI_SCAN_START", detail);
  if (connectSavedNetworks(NewoConfig::INITIAL_RECOVERY_WINDOW_MS)) {
    return;
  }

  Serial.println("[wifi] No saved network is reachable; opening browser provisioning");
  startProvisioning();
}

bool NewoWiFi::recoverLegacyNetwork() {
  wifi_config_t legacy = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &legacy) != ESP_OK || legacy.sta.ssid[0] == '\0') return false;

  const String ssid(reinterpret_cast<const char*>(legacy.sta.ssid),
                    strnlen(reinterpret_cast<const char*>(legacy.sta.ssid), sizeof(legacy.sta.ssid)));
  const String password(reinterpret_cast<const char*>(legacy.sta.password),
                        strnlen(reinterpret_cast<const char*>(legacy.sta.password), sizeof(legacy.sta.password)));
  if (ssid.length() > 32 || password.length() > 63) return false;
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::WIFI, "WIFI_LEGACY_RECOVERY_TRY");
  const NewoWifiCredential candidate{ssid, password};
  if (!connectToSavedNetwork(candidate, NewoConfig::WIFI_CONNECT_ATTEMPT_TIMEOUT_MS)) return false;
  if (!storage_.addOrUpdateNetwork(ssid, password)) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::STORAGE, "WIFI_LEGACY_RECOVERY_SAVE_FAILED");
    return true;
  }
  hasConnected_ = true;
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::STORAGE, "WIFI_LEGACY_RECOVERED");
  return true;
}

bool NewoWiFi::connectSavedNetworks(uint32_t windowMs) {
  if (storage_.count() == 0 || windowMs == 0) {
    return false;
  }

  const uint32_t deadlineMs = millis() + windowMs;
  lastReconnectAttemptMs_ = millis();

  do {
    if (scanAndConnect(deadlineMs)) {
      return true;
    }

    if (deadlineReached(deadlineMs)) {
      break;
    }

    delay(NewoConfig::WIFI_SCAN_RETRY_DELAY_MS);
  } while (!deadlineReached(deadlineMs));

  return false;
}

bool NewoWiFi::scanAndConnect(uint32_t deadlineMs) {
  // ESP32-S3 Wi-Fi is 2.4 GHz-only, so this scan covers the supported band.
  ++scanCount_;
  const int16_t found = WiFi.scanNetworks(false, false);
  std::vector<VisibleSavedNetwork> candidates;

  if (found > 0) {
    for (size_t savedIndex = 0; savedIndex < storage_.count(); ++savedIndex) {
      const String& savedSsid = storage_.networks()[savedIndex].ssid;
      bool visible = false;
      int32_t strongestRssi = -127;

      for (int16_t scanIndex = 0; scanIndex < found; ++scanIndex) {
        if (WiFi.SSID(scanIndex) != savedSsid) {
          continue;
        }

        visible = true;
        strongestRssi = std::max(strongestRssi, static_cast<int32_t>(WiFi.RSSI(scanIndex)));
      }

      if (visible) {
        candidates.push_back({savedIndex, strongestRssi});
      }
    }
  }
  WiFi.scanDelete();

  std::sort(candidates.begin(), candidates.end(),
            [](const VisibleSavedNetwork& left, const VisibleSavedNetwork& right) {
              if (left.rssi != right.rssi) {
                return left.rssi > right.rssi;
              }
              return left.savedIndex < right.savedIndex;
            });

  if (candidates.empty()) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::WIFI, "WIFI_SCAN_EMPTY");
    return false;
  }

  char resultDetail[48];
  snprintf(resultDetail, sizeof(resultDetail), "saved_networks=%u", static_cast<unsigned>(candidates.size()));
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::WIFI, "WIFI_SCAN_RESULT", resultDetail);
  for (const auto& candidate : candidates) {
    if (deadlineReached(deadlineMs)) {
      break;
    }

    const uint32_t remainingMs = deadlineMs - millis();
    const uint32_t timeoutMs = std::min(NewoConfig::WIFI_CONNECT_ATTEMPT_TIMEOUT_MS, remainingMs);
    if (timeoutMs < 100) {
      break;
    }

    if (connectToSavedNetwork(storage_.networks()[candidate.savedIndex], timeoutMs)) {
      hasConnected_ = true;
      return true;
    }
  }

  return false;
}

bool NewoWiFi::connectToSavedNetwork(const NewoWifiCredential& network, uint32_t timeoutMs) {
  ++connectAttemptCount_;
  char detail[96];
  snprintf(detail, sizeof(detail), "ssid=%s", network.ssid.c_str());
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::WIFI, "WIFI_CONNECTING", detail);
  WiFi.disconnect(false, false);

  if (network.password.length() == 0) {
    WiFi.begin(network.ssid.c_str());
  } else {
    WiFi.begin(network.ssid.c_str(), network.password.c_str());
  }

  const uint32_t deadlineMs = millis() + timeoutMs;
  while (!deadlineReached(deadlineMs)) {
    if (connected()) {
      ++connectSuccessCount_;
      char connectedDetail[96];
      snprintf(connectedDetail, sizeof(connectedDetail), "ssid=%s rssi=%d", WiFi.SSID().c_str(), WiFi.RSSI());
      NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::WIFI, "WIFI_CONNECTED", connectedDetail);
      return true;
    }
    delay(NewoConfig::WIFI_CONNECT_POLL_DELAY_MS);
  }

  ++connectFailureCount_;
  char failedDetail[96];
  snprintf(failedDetail, sizeof(failedDetail), "ssid=%s reason=%s", network.ssid.c_str(), wifiStatusName(WiFi.status()));
  NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::WIFI, "WIFI_CONNECT_FAILED", failedDetail);
  return false;
}

void NewoWiFi::startProvisioning() {
  if (provisioningAttempted_) {
    return;
  }

  provisioningAttempted_ = true;
  provisioningStartedAtMs_ = millis();
  provisioningActive_ = true;

  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(NewoConfig::PROVISIONING_DEVICE_NAME)) {
    provisioningActive_ = false;
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::PROV, "PROV_AP_FAILED");
    return;
  }
  char detail[64];
  snprintf(detail, sizeof(detail), "ssid=%s ip=%s", NewoConfig::PROVISIONING_DEVICE_NAME,
           WiFi.softAPIP().toString().c_str());
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::PROV, "PROV_STARTED", detail);
}

void NewoWiFi::stopProvisioning() {
  if (!provisioningActive_) {
    return;
  }

  WiFi.softAPdisconnect(true);
  provisioningActive_ = false;
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::PROV, "PROV_STOPPED");
}

void NewoWiFi::handleWiFiEvent(arduino_event_id_t eventId, const arduino_event_info_t& info) {
  switch (eventId) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      const auto reason = static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason);
      ++disconnectCount_;
      snprintf(lastDisconnectReason_, sizeof(lastDisconnectReason_), "%s", WiFi.STA.disconnectReasonName(reason));
      char detail[96];
      snprintf(detail, sizeof(detail), "reason=%s", lastDisconnectReason_);
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::WIFI, "WIFI_DISCONNECTED", detail);
      break;
    }

    default:
      break;
  }
}

bool NewoWiFi::provisioningTimedOut() const {
  return provisioningStartedAtMs_ != 0 &&
         static_cast<uint32_t>(millis() - provisioningStartedAtMs_) >=
             NewoConfig::BLE_PROVISIONING_TIMEOUT_MS;
}

bool NewoWiFi::deadlineReached(uint32_t deadlineMs) {
  return static_cast<int32_t>(millis() - deadlineMs) >= 0;
}

void NewoWiFi::loop() {
  if (provisioningActive_) {
    if (provisioningTimedOut()) {
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::PROV, "PROV_TIMEOUT");
      pendingLedEvent_ = LedEvent::TIMEOUT;
      stopProvisioning();
    }
    return;
  }

  if (connected()) {
    return;
  }

  if (storage_.count() == 0 || !hasConnected_) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastReconnectAttemptMs_ < NewoConfig::RECONNECT_INTERVAL_MS) {
    return;
  }

  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::WIFI, "WIFI_RECONNECTING");
  connectSavedNetworks(NewoConfig::RECONNECT_WINDOW_MS);
}

bool NewoWiFi::connected() const {
  return WiFi.status() == WL_CONNECTED;
}

String NewoWiFi::connectedSsid() const {
  return connected() ? WiFi.SSID() : String();
}

int32_t NewoWiFi::rssi() const {
  return connected() ? WiFi.RSSI() : 0;
}

uint32_t NewoWiFi::scanCount() const { return scanCount_; }
uint32_t NewoWiFi::connectAttemptCount() const { return connectAttemptCount_; }
uint32_t NewoWiFi::connectSuccessCount() const { return connectSuccessCount_; }
uint32_t NewoWiFi::connectFailureCount() const { return connectFailureCount_; }
uint32_t NewoWiFi::disconnectCount() const { return disconnectCount_; }
const char* NewoWiFi::lastDisconnectReason() const { return lastDisconnectReason_; }

NewoWiFi::LedEvent NewoWiFi::consumeLedEvent() {
  portENTER_CRITICAL(&provisioningMux_);
  const LedEvent event = pendingLedEvent_;
  pendingLedEvent_ = LedEvent::NONE;
  portEXIT_CRITICAL(&provisioningMux_);
  return event;
}
