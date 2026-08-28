#include "newo_cloud.h"

#include <ArduinoJson.h>
#include <cstring>
#include <esp_system.h>

#include "newo_config.h"
#include "newo_log.h"

#if __has_include("newo_secrets.h")
#include "newo_secrets.h"
#define NEWO_HAS_LOCAL_SECRETS 1
#else
#define NEWO_HAS_LOCAL_SECRETS 0
#endif

NewoCloud::NewoCloud(NewoWiFi& wifi) : wifi_(wifi) {}

void NewoCloud::begin() {
#if !NEWO_HAS_LOCAL_SECRETS
  NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "CLOUD_DISABLED", "secrets_missing");
  return;
#else
  if (strlen(NewoSecrets::DEVICE_ID) == 0 || strlen(NewoSecrets::DEVICE_SECRET) < 24) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "CLOUD_DISABLED", "identity_missing");
    return;
  }

  if (strlen(NewoSecrets::CLOUD_CA_CERT) < 100) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "CLOUD_DISABLED", "ca_missing");
    return;
  }

  webSocket_.onEvent(
      [this](WStype_t type, uint8_t* payload, size_t length) { handleEvent(type, payload, length); });
  webSocket_.setReconnectInterval(NewoConfig::CLOUD_RECONNECT_INTERVAL_MS);
  webSocket_.enableHeartbeat(NewoConfig::CLOUD_WS_PING_INTERVAL_MS,
                             NewoConfig::CLOUD_WS_PONG_TIMEOUT_MS,
                             NewoConfig::CLOUD_WS_MISSED_PONG_LIMIT);

  configured_ = true;
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::CLOUD, "CLOUD_CONFIGURED");
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
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::CLOUD, "CLOUD_CONNECTING");
#endif
}

void NewoCloud::loop() {
  if (rebootAtMs_ != 0 && static_cast<int32_t>(millis() - rebootAtMs_) >= 0) {
    NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::SYSTEM, "SYSTEM_REBOOTING");
    ESP.restart();
    return;
  }

  if (!configured_) {
    return;
  }

  if (!wifi_.connected()) {
    if (started_) {
      webSocket_.disconnect();
      started_ = false;
    }
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
      ++connectionCount_;
      NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::CLOUD, "CLOUD_CONNECTED");
      sendHello();
      sendStatus();
      break;

    case WStype_DISCONNECTED:
      if (connected_) {
        ++disconnectCount_;
        NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "CLOUD_DISCONNECTED");
      }
      connected_ = false;
      authenticated_ = false;
      started_ = false;
      break;

    case WStype_TEXT:
      handleTextMessage(payload, length);
      break;

    case WStype_ERROR:
      ++errorCount_;
      NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::CLOUD, "CLOUD_WS_ERROR");
      break;

    default:
      break;
  }
}

void NewoCloud::handleTextMessage(const uint8_t* payload, size_t length) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "CLOUD_INVALID_JSON");
    return;
  }

  const char* type = doc["type"] | "";

  if (strcmp(type, "hello_ack") == 0) {
    authenticated_ = true;
    NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::CLOUD, "CLOUD_AUTHENTICATED");
    return;
  }

  if (strcmp(type, "ping") == 0) {
    const char* requestId = doc["request_id"] | "";
    sendStatus(requestId, true);
    return;
  }

  if (strcmp(type, "status_request") == 0) {
    const char* requestId = doc["request_id"] | "";
    sendStatus(requestId, false);
    return;
  }

  if (strcmp(type, "reboot") == 0) {
    const char* requestId = doc["request_id"] | "";
    sendRebootAck(requestId);
    return;
  }

  if (strcmp(type, "health_request") == 0) {
    sendHealth(doc["request_id"] | "");
    return;
  }

  if (strcmp(type, "logs_request") == 0) {
    const uint8_t limit = constrain(doc["limit"] | 20, 1, 40);
    const char* minLevel = doc["min_level"] | "info";
    sendLogs(doc["request_id"] | "", limit, minLevel);
    return;
  }

  NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "CLOUD_UNSUPPORTED_MESSAGE");
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
  doc["ssid"] = wifi_.connectedSsid();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["free_psram"] = ESP.getFreePsram();

  String body;
  serializeJson(doc, body);
  webSocket_.sendTXT(body);
  lastStatusMs_ = millis();
}

void NewoCloud::sendHealth(const char* requestId) {
  if (!connected_ || !requestId || requestId[0] == '\0') return;

  const NewoLog::Snapshot logs = NewoLog::snapshot();
  JsonDocument doc;
  doc["type"] = "health";
  doc["request_id"] = requestId;
  doc["firmware"] = NewoConfig::FIRMWARE_VERSION;
  doc["chip"] = ESP.getChipModel();
  doc["uptime_ms"] = millis();
  doc["reset_reason"] = static_cast<uint32_t>(esp_reset_reason());
  doc["ssid"] = wifi_.connectedSsid();
  doc["rssi"] = wifi_.rssi();
  doc["cloud_connected"] = connected_ && authenticated_;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["min_free_heap"] = ESP.getMinFreeHeap();
  doc["free_psram"] = ESP.getFreePsram();
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["scans"] = wifi_.scanCount();
  wifi["connect_attempts"] = wifi_.connectAttemptCount();
  wifi["connections"] = wifi_.connectSuccessCount();
  wifi["failed"] = wifi_.connectFailureCount();
  wifi["disconnects"] = wifi_.disconnectCount();
  wifi["last_disconnect_reason"] = wifi_.lastDisconnectReason();
  JsonObject cloud = doc["cloud"].to<JsonObject>();
  cloud["connections"] = connectionCount_;
  cloud["disconnects"] = disconnectCount_;
  cloud["errors"] = errorCount_;
  JsonObject logger = doc["logs"].to<JsonObject>();
  logger["stored"] = logs.count;
  logger["capacity"] = NewoLog::kCapacity;
  logger["warnings"] = logs.warnings;
  logger["errors"] = logs.errors;

  String body;
  serializeJson(doc, body);
  webSocket_.sendTXT(body);
}

void NewoCloud::sendLogs(const char* requestId, uint8_t limit, const char* minLevel) {
  if (!connected_ || !requestId || requestId[0] == '\0') return;

  NewoLog::Level minimum = NewoLog::Level::INFO;
  if (strcmp(minLevel, "warn") == 0) minimum = NewoLog::Level::WARN;
  if (strcmp(minLevel, "error") == 0) minimum = NewoLog::Level::ERROR;
  const NewoLog::Snapshot snapshot = NewoLog::snapshot();

  JsonDocument doc;
  doc["type"] = "logs";
  doc["request_id"] = requestId;
  doc["firmware"] = NewoConfig::FIRMWARE_VERSION;
  doc["uptime_ms"] = millis();
  doc["warnings"] = snapshot.warnings;
  doc["errors"] = snapshot.errors;
  JsonArray entries = doc["entries"].to<JsonArray>();
  size_t first = snapshot.count;
  size_t selected = 0;
  for (size_t i = snapshot.count; i > 0 && selected < limit; --i) {
    if (static_cast<uint8_t>(snapshot.entries[i - 1].level) >= static_cast<uint8_t>(minimum)) {
      first = i - 1;
      ++selected;
    }
  }
  for (size_t i = first; i < snapshot.count; ++i) {
    const NewoLog::Entry& entry = snapshot.entries[i];
    if (static_cast<uint8_t>(entry.level) < static_cast<uint8_t>(minimum)) continue;
    JsonObject output = entries.add<JsonObject>();
    output["seq"] = entry.sequence;
    output["first_ms"] = entry.firstMs;
    output["last_ms"] = entry.lastMs;
    output["repeat"] = entry.repeat;
    output["level"] = NewoLog::levelName(entry.level);
    output["subsystem"] = NewoLog::subsystemName(entry.subsystem);
    output["code"] = entry.code;
    output["detail"] = entry.detail;
  }

  String body;
  serializeJson(doc, body);
  if (body.length() <= 16 * 1024) webSocket_.sendTXT(body);
}

void NewoCloud::sendRebootAck(const char* requestId) {
  if (!connected_ || !requestId || requestId[0] == '\0') {
    return;
  }

  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::SYSTEM, "SYSTEM_REBOOT_REQUESTED");
  JsonDocument doc;
  doc["type"] = "reboot_ack";
  doc["request_id"] = requestId;

  String body;
  serializeJson(doc, body);
  if (!webSocket_.sendTXT(body)) {
    NewoLog::log(NewoLog::Level::ERROR, NewoLog::Subsystem::CLOUD, "CLOUD_REBOOT_ACK_FAILED");
    return;
  }

  // sendTXT accepted the complete frame for transmission. Keep servicing the
  // socket for a short drain interval before restarting.
  rebootAtMs_ = millis() + NewoConfig::REMOTE_REBOOT_DELAY_MS;
}
