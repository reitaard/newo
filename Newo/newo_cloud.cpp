#include "newo_cloud.h"

#include <ArduinoJson.h>
#include <cstring>
#include <esp_system.h>
#include <esp_heap_caps.h>

#include "newo_config.h"
#include "newo_log.h"

#if __has_include("newo_secrets.h")
#include "newo_secrets.h"
#define NEWO_HAS_LOCAL_SECRETS 1
#else
#define NEWO_HAS_LOCAL_SECRETS 0
#endif

NewoCloud::NewoCloud(NewoWiFi& wifi, NewoDisplay& display) : wifi_(wifi), display_(display) {}

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

  recordStack("after wifi connection");
  String headers;
  headers.reserve(strlen(NewoSecrets::DEVICE_ID) + strlen(NewoSecrets::DEVICE_SECRET) + 64);
  headers += F("X-Newo-Device-Id: ");
  headers += NewoSecrets::DEVICE_ID;
  headers += F("\r\nAuthorization: Bearer ");
  headers += NewoSecrets::DEVICE_SECRET;

  webSocket_.setExtraHeaders(headers.c_str());
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

  if (!configured_) return;

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
  if (!connected_) return;

  const uint32_t now = millis();
  if (now - lastStatusMs_ >= NewoConfig::CLOUD_STATUS_INTERVAL_MS) sendStatus();
}

bool NewoCloud::connected() const { return connected_; }

bool NewoCloud::consumeVoiceRequest(VoiceRequest& request) {
  if (voiceRequestCount_ == 0) return false;
  request = voiceRequests_[voiceRequestHead_];
  voiceRequestHead_ = (voiceRequestHead_ + 1) % kVoiceRequestQueueDepth;
  --voiceRequestCount_;
  return true;
}

bool NewoCloud::consumeSpeakerRequest(SpeakerRequest& request) {
  if (speakerRequestCount_ == 0) return false;
  request = speakerRequests_[speakerRequestHead_];
  speakerRequestHead_ = (speakerRequestHead_ + 1) % kSpeakerRequestQueueDepth;
  --speakerRequestCount_;
  return true;
}

void NewoCloud::sendSpeakerResult(const char* playbackId, bool success, uint32_t bytes, const char* error) {
  if (!connected_ || !playbackId || !playbackId[0]) return;
  JsonDocument doc;
  doc["type"] = success ? "speaker_complete" : "speaker_error";
  doc["playback_id"] = playbackId;
  doc["bytes"] = bytes;
  if (!success) doc["error"] = error && error[0] ? error : "unknown";
  String body; serializeJson(doc, body); webSocket_.sendTXT(body);
}

void NewoCloud::updateVoiceTelemetry(NewoVoiceState state, bool connected, uint32_t wakes,
                                      uint32_t sessions, uint32_t failures, uint32_t timeouts) {
  voiceState_ = state; voiceConnected_ = connected; voiceWakes_ = wakes;
  voiceSessions_ = sessions; voiceFailures_ = failures; voiceTimeouts_ = timeouts;
}

void NewoCloud::sendVoiceAck(const char* requestId, NewoVoiceState state, bool voiceConnected,
                             uint32_t wakes, uint32_t sessions) {
  if (!connected_ || !requestId || !requestId[0]) return;
  JsonDocument doc;
  doc["type"] = "voice_ack";
  doc["request_id"] = requestId;
  doc["state"] = newoVoiceStateName(state);
  doc["voice_connected"] = voiceConnected;
  doc["wake_count"] = wakes;
  doc["session_count"] = sessions;
  String body; serializeJson(doc, body); webSocket_.sendTXT(body);
}

void NewoCloud::recordStack(const char* point) {
  const uint32_t bytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  if (bytes < minimumLoopStackBytes_) minimumLoopStackBytes_ = bytes;
  Serial.printf("[stack] %s: %lu bytes free (low %lu)\n", point,
                static_cast<unsigned long>(bytes),
                static_cast<unsigned long>(minimumLoopStackBytes_));
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
    recordStack("cloud authenticated");
    return;
  }

  if (strcmp(type, "ping") == 0) {
    sendStatus(doc["request_id"] | "", true);
    return;
  }

  if (strcmp(type, "status_request") == 0) {
    sendStatus(doc["request_id"] | "", false);
    return;
  }

  if (strcmp(type, "reboot") == 0) {
    sendRebootAck(doc["request_id"] | "");
    return;
  }

  if (strcmp(type, "display_set") == 0) {
    const char* requestId = doc["request_id"] | "";
    const char* mode = doc["mode"] | "";
    const char* text = doc["text"] | "";

    bool faceSelection = true;
    NewoFaceStyle faceStyle = NewoFaceStyle::NEUTRAL;
    if (strcmp(mode, "default") == 0) faceStyle = NewoFaceStyle::NEUTRAL;
    else if (strcmp(mode, "happy") == 0) faceStyle = NewoFaceStyle::HAPPY;
    else if (strcmp(mode, "angry") == 0) faceStyle = NewoFaceStyle::ANGRY;
    else if (strcmp(mode, "tired") == 0) faceStyle = NewoFaceStyle::TIRED;
    else if (strcmp(mode, "curious") == 0) faceStyle = NewoFaceStyle::CURIOUS;
    else if (strcmp(mode, "confused") == 0) faceStyle = NewoFaceStyle::CONFUSED;
    else if (strcmp(mode, "laugh") == 0) faceStyle = NewoFaceStyle::LAUGH;
    else if (strcmp(mode, "sweat") == 0) faceStyle = NewoFaceStyle::SWEAT;
    else if (strcmp(mode, "cyclops") == 0) faceStyle = NewoFaceStyle::CYCLOPS;
    else faceSelection = false;

    if (faceSelection) {
      if (!display_.setFaceStyle(faceStyle)) {
        NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "DISPLAY_INVALID_PAYLOAD");
        return;
      }
      sendDisplayAck(requestId, mode);
      return;
    }

    NewoDisplayMode displayMode = NewoDisplayMode::MESSAGE;
    if (strcmp(mode, "idle") == 0) displayMode = NewoDisplayMode::IDLE;
    else if (strcmp(mode, "listening") == 0) displayMode = NewoDisplayMode::LISTENING;
    else if (strcmp(mode, "thinking") == 0) displayMode = NewoDisplayMode::THINKING;
    else if (strcmp(mode, "speaking") == 0) displayMode = NewoDisplayMode::SPEAKING;
    else if (strcmp(mode, "error") == 0) displayMode = NewoDisplayMode::ERROR;
    else if (strcmp(mode, "message") != 0) {
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "DISPLAY_INVALID_MODE");
      return;
    }
    if (strlen(text) > 96 || !display_.setMode(displayMode, text, doc["temporary"] | false)) {
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "DISPLAY_INVALID_PAYLOAD");
      return;
    }
    sendDisplayAck(requestId, mode);
    return;
  }

  if (strcmp(type, "eco_toggle") == 0) {
    display_.toggleEco();
    sendDisplayAck(doc["request_id"] | "", display_.ecoEnabled() ? "eco_on" : "eco_off");
    return;
  }

  if (strcmp(type, "speaker_play") == 0) {
    const char* playbackId = doc["playback_id"] | "";
    SpeakerRequest request = {};
    strlcpy(request.playbackId, playbackId, sizeof(request.playbackId));
    request.sampleRate = doc["sample_rate"] | 0;
    request.channels = doc["channels"] | 0;
    request.bitsPerSample = doc["bits_per_sample"] | 0;
    request.bytes = doc["bytes"] | 0;
    if (strlen(playbackId) != 36 || !request.bytes || speakerRequestCount_ == kSpeakerRequestQueueDepth) {
      sendSpeakerResult(playbackId, false, 0, speakerRequestCount_ == kSpeakerRequestQueueDepth ? "busy" : "invalid_request");
      return;
    }
    speakerRequests_[speakerRequestTail_] = request;
    speakerRequestTail_ = (speakerRequestTail_ + 1) % kSpeakerRequestQueueDepth;
    ++speakerRequestCount_;
    return;
  }

  if (strcmp(type, "voice_status") == 0) {
    sendVoiceAck(doc["request_id"] | "", voiceState_, voiceConnected_, voiceWakes_, voiceSessions_);
    return;
  }

  if (strcmp(type, "voice_control") == 0) {
    const char* requestId = doc["request_id"] | "";
    const char* action = doc["action"] | "";
    if (!requestId[0]) return;
    VoiceRequest request = {};
    if (strcmp(action, "on") == 0) request.action = VoiceRequest::Action::ON;
    else if (strcmp(action, "off") == 0) request.action = VoiceRequest::Action::OFF;
    else if (strcmp(action, "toggle") == 0) request.action = VoiceRequest::Action::TOGGLE;
    else {
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "VOICE_INVALID_ACTION");
      return;
    }
    if (voiceRequestCount_ == kVoiceRequestQueueDepth) {
      NewoLog::log(NewoLog::Level::WARN, NewoLog::Subsystem::CLOUD, "VOICE_CONTROL_QUEUE_FULL");
      return;
    }
    strlcpy(request.requestId, requestId, sizeof(request.requestId));
    voiceRequests_[voiceRequestTail_] = request;
    voiceRequestTail_ = (voiceRequestTail_ + 1) % kVoiceRequestQueueDepth;
    ++voiceRequestCount_;
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
  if (!connected_) return;
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
  if (!connected_) return;
  JsonDocument doc;
  doc["type"] = pong ? "pong" : "status";
  if (requestId && requestId[0] != '\0') doc["request_id"] = requestId;
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
  recordStack("before health");
  const NewoLog::Stats logs = NewoLog::stats();
  recordStack("during health");
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
  doc["largest_free_internal_block"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  JsonObject voice = doc["voice"].to<JsonObject>();
  voice["state"] = newoVoiceStateName(voiceState_);
  voice["connected"] = voiceConnected_;
  voice["wakes"] = voiceWakes_;
  voice["sessions"] = voiceSessions_;
  voice["failures"] = voiceFailures_;
  voice["timeouts"] = voiceTimeouts_;
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
  doc["loop_stack_low_bytes"] = minimumLoopStackBytes_;
  String body;
  serializeJson(doc, body);
  webSocket_.sendTXT(body);
  recordStack("after health");
}

void NewoCloud::sendLogs(const char* requestId, uint8_t limit, const char* minLevel) {
  if (!connected_ || !requestId || requestId[0] == '\0') return;
  recordStack("before logs");
  NewoLog::Level minimum = NewoLog::Level::INFO;
  if (strcmp(minLevel, "warn") == 0) minimum = NewoLog::Level::WARN;
  if (strcmp(minLevel, "error") == 0) minimum = NewoLog::Level::ERROR;

  const size_t exportBytes = static_cast<size_t>(limit) * sizeof(NewoLog::Entry);
  NewoLog::Entry* entries = static_cast<NewoLog::Entry*>(heap_caps_malloc(exportBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!entries) entries = static_cast<NewoLog::Entry*>(malloc(exportBytes));

  NewoLog::Stats logStats = {};
  if (!entries) {
    JsonDocument failed;
    failed["type"] = "logs";
    failed["request_id"] = requestId;
    failed["firmware"] = NewoConfig::FIRMWARE_VERSION;
    failed["uptime_ms"] = millis();
    const NewoLog::Stats current = NewoLog::stats();
    failed["warnings"] = current.warnings;
    failed["errors"] = current.errors;
    failed["error"] = "allocation_failed";
    failed["entries"].to<JsonArray>();
    String body;
    serializeJson(failed, body);
    webSocket_.sendTXT(body);
    recordStack("logs allocation failed");
    return;
  }

  const size_t count = NewoLog::copyRecent(entries, limit, minimum, &logStats);
  recordStack("during logs");
  JsonDocument doc;
  doc["type"] = "logs";
  doc["request_id"] = requestId;
  doc["firmware"] = NewoConfig::FIRMWARE_VERSION;
  doc["uptime_ms"] = millis();
  doc["warnings"] = logStats.warnings;
  doc["errors"] = logStats.errors;
  JsonArray outputEntries = doc["entries"].to<JsonArray>();
  for (size_t i = 0; i < count; ++i) {
    const NewoLog::Entry& entry = entries[i];
    JsonObject output = outputEntries.add<JsonObject>();
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
  heap_caps_free(entries);
  recordStack("after logs");
}

void NewoCloud::sendDisplayAck(const char* requestId, const char* mode) {
  if (!connected_ || !requestId || requestId[0] == '\0') return;
  JsonDocument doc;
  doc["type"] = "display_ack";
  doc["request_id"] = requestId;
  doc["mode"] = mode;
  String body; serializeJson(doc, body); webSocket_.sendTXT(body);
}

void NewoCloud::sendRebootAck(const char* requestId) {
  if (!connected_ || !requestId || requestId[0] == '\0') return;
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
  rebootAtMs_ = millis() + NewoConfig::REMOTE_REBOOT_DELAY_MS;
}
