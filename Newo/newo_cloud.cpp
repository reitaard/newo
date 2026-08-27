#include "newo_cloud.h"

#include <ArduinoJson.h>
#include <cstring>

#include "newo_config.h"

#if __has_include("newo_secrets.h")
#include "newo_secrets.h"
#define NEWO_HAS_LOCAL_SECRETS 1
#else
#define NEWO_HAS_LOCAL_SECRETS 0
#endif

NewoCloud::NewoCloud(NewoWiFi& wifi) : wifi_(wifi) {}

void NewoCloud::begin() {
#if !NEWO_HAS_LOCAL_SECRETS
  Serial.println("[cloud] Disabled: Newo/newo_secrets.h is missing");
  Serial.println("[cloud] Copy newo_secrets.example.h locally and provision the device secret + trusted CA");
  return;
#else
  if (strlen(NewoSecrets::DEVICE_ID) == 0 || strlen(NewoSecrets::DEVICE_SECRET) < 24) {
    Serial.println("[cloud] Disabled: device identity/secret is not provisioned");
    return;
  }

  if (strlen(NewoSecrets::CLOUD_CA_CERT) < 100) {
    Serial.println("[cloud] Disabled: trusted cloud CA certificate is not provisioned");
    return;
  }

  webSocket_.onEvent(
      [this](WStype_t type, uint8_t* payload, size_t length) { handleEvent(type, payload, length); });
  webSocket_.setReconnectInterval(NewoConfig::CLOUD_RECONNECT_INTERVAL_MS);
  webSocket_.enableHeartbeat(NewoConfig::CLOUD_WS_PING_INTERVAL_MS,
                             NewoConfig::CLOUD_WS_PONG_TIMEOUT_MS,
                             NewoConfig::CLOUD_WS_MISSED_PONG_LIMIT);

  configured_ = true;
  Serial.printf("[cloud] Configured for wss://%s%s\n", NewoConfig::CLOUD_HOST,
                NewoConfig::CLOUD_PATH);
#endif
}

void NewoCloud::startConnection() {
#if NEWO_HAS_LOCAL_SECRETS
  if (!configured_ || started_ || !wifi_.connected()) {
    return;
  }

  String headers;
  headers.reserve(strlen(NewoSecrets::DEVICE_ID) + strlen(NewoSecrets::DEVICE_SECRET) + 64);
  headers += F("X-Newo-Device-Id: ");
  headers += NewoSecrets::DEVICE_ID;
  headers += F("\r\nAuthorization: Bearer ");
  headers += NewoSecrets::DEVICE_SECRET;

  // WebSockets 2.7.2 copies the extra headers into its client state.
  // Never print `headers`: it contains the device bearer secret.
  webSocket_.setExtraHeaders(headers.c_str());

  // Deliberately use beginSslWithCA instead of beginSSL. On ESP32 the latter
  // falls back to setInsecure() when no trust anchor is supplied.
  webSocket_.beginSslWithCA(NewoConfig::CLOUD_HOST, NewoConfig::CLOUD_PORT,
                            NewoConfig::CLOUD_PATH, NewoSecrets::CLOUD_CA_CERT, "");
  started_ = true;
  Serial.println("[cloud] Starting certificate-validated WSS connection...");
#endif
}

void NewoCloud::loop() {
  if (!configured_) {
    return;
  }

  if (!wifi_.connected()) {
    connected_ = false;
    return;
  }

  startConnection();
  webSocket_.loop();

  if (!connected_) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastStatusMs_ >= NewoConfig::CLOUD_STATUS_INTERVAL_MS) {
    sendStatus();
  }
}

bool NewoCloud::connected() const {
  return connected_;
}

void NewoCloud::handleEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      connected_ = true;
      Serial.printf("[cloud] Connected: wss://%s%s\n", NewoConfig::CLOUD_HOST,
                    NewoConfig::CLOUD_PATH);
      sendHello();
      sendStatus();
      break;

    case WStype_DISCONNECTED:
      if (connected_) {
        Serial.println("[cloud] Disconnected; automatic reconnect is enabled");
      }
      connected_ = false;
      break;

    case WStype_TEXT:
      handleTextMessage(payload, length);
      break;

    case WStype_ERROR:
      Serial.println("[cloud] WebSocket error");
      break;

    default:
      break;
  }
}

void NewoCloud::handleTextMessage(const uint8_t* payload, size_t length) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.printf("[cloud] Ignoring invalid server JSON: %s\n", error.c_str());
    return;
  }

  const char* type = doc["type"] | "";

  if (strcmp(type, "hello_ack") == 0) {
    Serial.println("[cloud] Server authenticated Newo");
    return;
  }

  if (strcmp(type, "ping") == 0) {
    // Use a string fallback so ArduinoJson converts the request ID correctly.
    const char* requestId = doc["request_id"] | "";
    sendStatus(requestId, true);
    return;
  }

  if (strcmp(type, "status_request") == 0) {
    const char* requestId = doc["request_id"] | "";
    sendStatus(requestId, false);
    return;
  }

  if (strcmp(type, "setup_wifi") == 0) {
    const char* requestId = doc["request_id"] | "";
    if (requestId[0] != '\0') {
      sendSetupWifiResult(requestId);
    }
    return;
  }

  Serial.printf("[cloud] Ignoring unsupported server message: %s\n", type);
}

void NewoCloud::sendHello() {
#if NEWO_HAS_LOCAL_SECRETS
  if (!connected_) {
    return;
  }

  JsonDocument doc;
  doc["type"] = "hello";
  doc["device"] = NewoSecrets::DEVICE_ID;
  doc["firmware"] = NewoConfig::FIRMWARE_VERSION;
  doc["chip"] = ESP.getChipModel();

  String body;
  serializeJson(doc, body);
  webSocket_.sendTXT(body);
#endif
}

void NewoCloud::sendStatus(const char* requestId, bool pong) {
  if (!connected_) {
    return;
  }

  JsonDocument doc;
  doc["type"] = pong ? "pong" : "status";
  if (requestId && requestId[0] != '\0') {
    doc["request_id"] = requestId;
  }
  doc["uptime_ms"] = millis();
  doc["rssi"] = wifi_.rssi();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["free_psram"] = ESP.getFreePsram();

  String body;
  serializeJson(doc, body);
  webSocket_.sendTXT(body);
  lastStatusMs_ = millis();
}

void NewoCloud::sendSetupWifiResult(const char* requestId) {
  if (!connected_ || !requestId || requestId[0] == '\0') {
    return;
  }

  const bool active = wifi_.startTemporarySetupAP(NewoConfig::SETUP_AP_TIMEOUT_MS);

  JsonDocument doc;
  doc["type"] = "setup_wifi_result";
  doc["request_id"] = requestId;
  doc["active"] = active;

  if (active) {
    doc["ssid"] = NewoConfig::SETUP_AP_SSID;
    doc["password"] = wifi_.setupApPassword();
    doc["url"] = String("http://") + wifi_.setupIP().toString();
    doc["timeout_s"] = wifi_.setupApRemainingSeconds();
  }

  String body;
  serializeJson(doc, body);
  webSocket_.sendTXT(body);
}
