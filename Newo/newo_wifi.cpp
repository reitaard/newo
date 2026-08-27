#include "newo_wifi.h"

#include <algorithm>
#include <cstring>

#include "newo_config.h"

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
    Serial.println("[wifi] No saved networks; opening BLE provisioning");
    startBleProvisioning();
    return;
  }

  Serial.printf("[wifi] Scanning for %u saved network(s)...\n",
                static_cast<unsigned>(storage_.count()));
  if (connectSavedNetworks(NewoConfig::INITIAL_RECOVERY_WINDOW_MS)) {
    return;
  }

  Serial.println("[wifi] No saved network is reachable; opening BLE provisioning");
  startBleProvisioning();
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
    Serial.println("[wifi] Scan found no saved network");
    return false;
  }

  Serial.printf("[wifi] Found %u saved network(s)\n",
                static_cast<unsigned>(candidates.size()));
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
  Serial.printf("[wifi] Trying saved network: %s\n", network.ssid.c_str());
  WiFi.disconnect(false, false);

  if (network.password.length() == 0) {
    WiFi.begin(network.ssid.c_str());
  } else {
    WiFi.begin(network.ssid.c_str(), network.password.c_str());
  }

  const uint32_t deadlineMs = millis() + timeoutMs;
  while (!deadlineReached(deadlineMs)) {
    if (connected()) {
      Serial.printf("[wifi] Connected: %s\n", WiFi.SSID().c_str());
      Serial.printf("[wifi] IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("[wifi] RSSI: %d dBm\n", WiFi.RSSI());
      return true;
    }
    delay(NewoConfig::WIFI_CONNECT_POLL_DELAY_MS);
  }

  Serial.printf("[wifi] Connection failed: %s (%s)\n",
                network.ssid.c_str(), wifiStatusName(WiFi.status()));
  return false;
}

void NewoWiFi::startBleProvisioning() {
  if (provisioningAttempted_ || rebootAtMs_ != 0) {
    return;
  }

  provisioningAttempted_ = true;
  provisioningStartedAtMs_ = millis();
  provisioningActive_ = true;

  // Security 1 keeps the BLE session encrypted. A null PoP is intentional for
  // this prototype; a display can provide a per-device PoP and QR flow later.
  WiFiProv.beginProvision(
      NETWORK_PROV_SCHEME_BLE,
      NETWORK_PROV_SCHEME_HANDLER_FREE_BLE,
      NETWORK_PROV_SECURITY_1,
      nullptr,
      NewoConfig::PROVISIONING_DEVICE_NAME,
      nullptr,
      nullptr,
      true);
  Serial.printf("[prov] BLE provisioning started: %s (timeout %lu s)\n",
                NewoConfig::PROVISIONING_DEVICE_NAME,
                static_cast<unsigned long>(NewoConfig::BLE_PROVISIONING_TIMEOUT_MS / 1000));
}

void NewoWiFi::stopBleProvisioning() {
  if (!provisioningActive_) {
    return;
  }

  WiFiProv.endProvision();
  provisioningActive_ = false;
  Serial.println("[prov] BLE provisioning stopped");
}

void NewoWiFi::handleWiFiEvent(arduino_event_id_t eventId, const arduino_event_info_t& info) {
  switch (eventId) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      const auto reason = static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason);
      Serial.printf("[wifi] STA disconnect reason: %u (%s)\n",
                    static_cast<unsigned>(reason), WiFi.STA.disconnectReasonName(reason));
      break;
    }

    case ARDUINO_EVENT_PROV_START:
      Serial.println("[prov] BLE service is ready; use ESP BLE Provisioning");
      break;

    case ARDUINO_EVENT_PROV_CRED_RECV:
      portENTER_CRITICAL(&provisioningMux_);
      copyProvisioningField(
          pendingProvisioningSsid_, sizeof(pendingProvisioningSsid_),
          info.prov_cred_recv.ssid,
          sizeof(info.prov_cred_recv.ssid));
      copyProvisioningField(
          pendingProvisioningPassword_, sizeof(pendingProvisioningPassword_),
          info.prov_cred_recv.password,
          sizeof(info.prov_cred_recv.password));
      provisioningCredentialsPending_ = true;
      provisioningCredentialsSucceeded_ = false;
      portEXIT_CRITICAL(&provisioningMux_);
      Serial.println("[prov] Wi-Fi credentials received; waiting for connection success");
      break;

    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      portENTER_CRITICAL(&provisioningMux_);
      provisioningCredentialsSucceeded_ = true;
      portEXIT_CRITICAL(&provisioningMux_);
      Serial.println("[prov] Wi-Fi credentials accepted");
      break;

    case ARDUINO_EVENT_PROV_CRED_FAIL:
      portENTER_CRITICAL(&provisioningMux_);
      provisioningCredentialsPending_ = false;
      provisioningCredentialsSucceeded_ = false;
      portEXIT_CRITICAL(&provisioningMux_);
      Serial.printf("[prov] Wi-Fi credentials rejected (reason %u)\n",
                    static_cast<unsigned>(info.prov_fail_reason));
      break;

    case ARDUINO_EVENT_PROV_END:
      Serial.println("[prov] BLE provisioning service ended");
      break;

    default:
      break;
  }
}

void NewoWiFi::processProvisioningHandoff() {
  char ssid[sizeof(pendingProvisioningSsid_)] = {};
  char password[sizeof(pendingProvisioningPassword_)] = {};
  bool commit = false;

  portENTER_CRITICAL(&provisioningMux_);
  if (provisioningCredentialsPending_ && provisioningCredentialsSucceeded_) {
    memcpy(ssid, pendingProvisioningSsid_, sizeof(ssid));
    memcpy(password, pendingProvisioningPassword_, sizeof(password));
    provisioningCredentialsPending_ = false;
    provisioningCredentialsSucceeded_ = false;
    commit = true;
  }
  portEXIT_CRITICAL(&provisioningMux_);

  if (!commit) {
    return;
  }

  if (!storage_.addOrUpdateNetwork(String(ssid), String(password))) {
    // The provisioning framework has already accepted the network. Preserve
    // the old transactional list, close BLE, and reboot instead of remaining
    // in an ambiguous successful-but-unsaved session.
    Serial.println("[prov] Could not save provisioned Wi-Fi network; rebooting without it");
    stopBleProvisioning();
    scheduleReboot();
    return;
  }

  stopBleProvisioning();
  Serial.println("[prov] Wi-Fi network saved; rebooting Newo");
  scheduleReboot();
}

bool NewoWiFi::provisioningTimedOut() const {
  return provisioningStartedAtMs_ != 0 &&
         static_cast<uint32_t>(millis() - provisioningStartedAtMs_) >=
             NewoConfig::BLE_PROVISIONING_TIMEOUT_MS;
}

void NewoWiFi::scheduleReboot() {
  rebootAtMs_ = millis() + NewoConfig::PROVISIONING_REBOOT_DELAY_MS;
}

bool NewoWiFi::deadlineReached(uint32_t deadlineMs) {
  return static_cast<int32_t>(millis() - deadlineMs) >= 0;
}

void NewoWiFi::copyProvisioningField(char* destination, size_t destinationSize,
                                      const uint8_t* source, size_t sourceSize) {
  if (!destination || destinationSize == 0 || !source) {
    return;
  }

  size_t length = 0;
  while (length + 1 < destinationSize && length < sourceSize && source[length] != '\0') {
    destination[length] = static_cast<char>(source[length]);
    ++length;
  }
  destination[length] = '\0';
}

void NewoWiFi::loop() {
  processProvisioningHandoff();

  if (rebootAtMs_ != 0) {
    if (deadlineReached(rebootAtMs_)) {
      Serial.println("[prov] Restarting Newo...");
      ESP.restart();
    }
    return;
  }

  if (provisioningActive_) {
    if (provisioningTimedOut()) {
      Serial.println("[prov] BLE provisioning timed out; reboot to try again");
      stopBleProvisioning();
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

  Serial.println("[wifi] Reconnecting to saved networks...");
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
