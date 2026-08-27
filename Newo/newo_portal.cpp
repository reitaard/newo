#include "newo_portal.h"

#include <ArduinoJson.h>
#include <WiFi.h>

#include "newo_config.h"

NewoPortal::NewoPortal(NewoStorage& storage, NewoWiFi& wifi)
    : storage_(storage), wifi_(wifi) {}

void NewoPortal::begin() {
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  server_.on("/api/wifi/scan", HTTP_GET, [this]() { handleScan(); });
  server_.on("/wifi/save", HTTP_POST, [this]() { handleSaveNetwork(); });
  server_.on("/wifi/delete", HTTP_POST, [this]() { handleDeleteNetwork(); });
  server_.on("/wifi/clear", HTTP_POST, [this]() { handleClearNetworks(); });
  server_.onNotFound([this]() { handleNotFound(); });

  server_.begin();
  Serial.println("[portal] HTTP server started");
  syncDnsState();
}

void NewoPortal::syncDnsState() {
  if (wifi_.setupApActive() && !dnsRunning_) {
    if (dnsServer_.start(53, "*", wifi_.setupIP())) {
      dnsRunning_ = true;
      Serial.println("[portal] Captive DNS started");
    } else {
      Serial.println("[portal] Captive DNS failed to start");
    }
  }

  if (!wifi_.setupApActive() && dnsRunning_) {
    dnsServer_.stop();
    dnsRunning_ = false;
    Serial.println("[portal] Captive DNS stopped");
  }
}

void NewoPortal::loop() {
  syncDnsState();

  if (dnsRunning_) {
    dnsServer_.processNextRequest();
  }

  server_.handleClient();

  if (rebootScheduled_ && static_cast<int32_t>(millis() - rebootAtMs_) >= 0) {
    Serial.println("[portal] Rebooting Newo...");
    delay(50);
    ESP.restart();
  }
}

String NewoPortal::htmlEscape(const String& value) const {
  String escaped;
  escaped.reserve(value.length() + 8);

  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&': escaped += F("&amp;"); break;
      case '<': escaped += F("&lt;"); break;
      case '>': escaped += F("&gt;"); break;
      case '"': escaped += F("&quot;"); break;
      case 39: escaped += F("&#39;"); break;
      default: escaped += value[i]; break;
    }
  }

  return escaped;
}

String NewoPortal::buildHomePage() {
  String html;
  html.reserve(9000);

  html += F("<!doctype html><html><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Newo Setup</title><style>");
  html += F("body{font-family:system-ui,-apple-system,sans-serif;max-width:760px;margin:32px auto;padding:0 18px;background:#f5f5f5;color:#161616}");
  html += F("h1{margin-bottom:4px}.muted{color:#666}.card{background:#fff;border:1px solid #ddd;border-radius:12px;padding:16px;margin:14px 0}");
  html += F("input,button{font:inherit;padding:10px;border-radius:8px;border:1px solid #bbb}input{box-sizing:border-box;width:100%;margin:5px 0 10px}");
  html += F("button{cursor:pointer;background:#111;color:#fff}.danger{background:#fff;color:#a00}.network{padding:12px 0;border-top:1px solid #eee}.network:first-child{border-top:0}");
  html += F(".row{display:flex;gap:8px;align-items:center;justify-content:space-between}.tag{font-size:12px;color:#666}");
  html += F("</style></head><body>");
  html += F("<h1>Newo</h1><div class='muted'>ESP32-S3 setup and network control</div>");

  html += F("<div class='card'><strong>Status</strong><p>");
  if (wifi_.connected()) {
    html += F("Connected to <b>");
    html += htmlEscape(wifi_.connectedSsid());
    html += F("</b><br>IP: ");
    html += wifi_.localIP().toString();
    html += F("<br>Signal: ");
    html += String(wifi_.rssi());
    html += F(" dBm<br>Local name: <b>newo.local</b>");
  } else {
    html += F("Not connected to the Internet.");
  }

  if (wifi_.setupApActive()) {
    html += F("<br>Setup AP: <b>");
    html += NewoConfig::SETUP_AP_SSID;
    html += F("</b><br>Setup IP: ");
    html += wifi_.setupIP().toString();
  }
  html += F("</p></div>");

  html += F("<div class='card'><strong>Nearby Wi-Fi</strong>");
  const int16_t found = WiFi.scanNetworks();

  if (found <= 0) {
    html += F("<p class='muted'>No compatible networks found.</p>");
  } else {
    for (int16_t i = 0; i < found; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) {
        continue;
      }

      html += F("<div class='network'><div class='row'><b>");
      html += htmlEscape(ssid);
      html += F("</b><span class='tag'>");
      html += String(WiFi.RSSI(i));
      html += F(" dBm · ");
      html += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? F("open") : F("secured");
      html += F("</span></div><form method='post' action='/wifi/save'>");
      html += F("<input type='hidden' name='ssid' value=\"");
      html += htmlEscape(ssid);
      html += F("\"><input type='password' name='password' maxlength='63' placeholder='Wi-Fi password (blank for open network)'>");
      html += F("<button type='submit'>Save & connect</button></form></div>");
    }
  }
  WiFi.scanDelete();
  html += F("</div>");

  html += F("<div class='card'><strong>Add network manually</strong><form method='post' action='/wifi/save'>");
  html += F("<input name='ssid' maxlength='31' placeholder='SSID' required>");
  html += F("<input type='password' name='password' maxlength='63' placeholder='Password (blank for open network)'>");
  html += F("<button type='submit'>Save & connect</button></form></div>");

  html += F("<div class='card'><strong>Saved networks</strong>");
  if (storage_.count() == 0) {
    html += F("<p class='muted'>None yet.</p>");
  } else {
    for (const auto& network : storage_.networks()) {
      html += F("<div class='network'><div class='row'><span>");
      html += htmlEscape(network.ssid);
      html += F("</span><form method='post' action='/wifi/delete'><input type='hidden' name='ssid' value=\"");
      html += htmlEscape(network.ssid);
      html += F("\"><button class='danger' type='submit'>Remove</button></form></div></div>");
    }

    html += F("<form method='post' action='/wifi/clear' onsubmit=\"return confirm('Remove every saved Wi-Fi network?')\">");
    html += F("<button class='danger' type='submit'>Clear all saved networks</button></form>");
  }
  html += F("</div>");

  html += F("<p class='muted'>API: <a href='/api/status'>/api/status</a> · <a href='/api/wifi/scan'>/api/wifi/scan</a></p>");
  html += F("</body></html>");
  return html;
}

void NewoPortal::handleRoot() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/html; charset=utf-8", buildHomePage());
}

void NewoPortal::handleStatus() {
  JsonDocument doc;
  doc["device"] = NewoConfig::DEVICE_NAME;
  doc["chip"] = ESP.getChipModel();
  doc["connected"] = wifi_.connected();
  doc["setup_ap"] = wifi_.setupApActive();
  doc["saved_networks"] = storage_.count();
  doc["uptime_ms"] = millis();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["free_psram"] = ESP.getFreePsram();

  if (wifi_.connected()) {
    doc["ssid"] = wifi_.connectedSsid();
    doc["ip"] = wifi_.localIP().toString();
    doc["rssi"] = wifi_.rssi();
  }

  if (wifi_.setupApActive()) {
    doc["setup_ip"] = wifi_.setupIP().toString();
  }

  String body;
  serializeJson(doc, body);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", body);
}

void NewoPortal::handleScan() {
  JsonDocument doc;
  JsonArray networks = doc["networks"].to<JsonArray>();

  const int16_t found = WiFi.scanNetworks();
  if (found > 0) {
    for (int16_t i = 0; i < found; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) {
        continue;
      }

      JsonObject item = networks.add<JsonObject>();
      item["ssid"] = ssid;
      item["rssi"] = WiFi.RSSI(i);
      item["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
  }
  WiFi.scanDelete();

  String body;
  serializeJson(doc, body);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "application/json", body);
}

void NewoPortal::handleSaveNetwork() {
  if (!server_.hasArg("ssid")) {
    server_.send(400, "text/plain", "Missing SSID");
    return;
  }

  const String ssid = server_.arg("ssid");
  const String password = server_.hasArg("password") ? server_.arg("password") : String();

  if (!storage_.addOrUpdateNetwork(ssid, password)) {
    server_.send(400, "text/plain", "Could not save network. Check SSID/password length or saved-network limit.");
    return;
  }

  server_.send(200, "text/html; charset=utf-8",
               "<html><body style='font-family:system-ui'><h2>Saved</h2><p>Newo will reboot and try the saved networks.</p></body></html>");
  scheduleReboot();
}

void NewoPortal::handleDeleteNetwork() {
  if (!server_.hasArg("ssid") || !storage_.removeNetwork(server_.arg("ssid"))) {
    server_.send(404, "text/plain", "Saved network not found");
    return;
  }

  server_.send(200, "text/html; charset=utf-8",
               "<html><body style='font-family:system-ui'><h2>Removed</h2><p>Newo will reboot.</p></body></html>");
  scheduleReboot();
}

void NewoPortal::handleClearNetworks() {
  if (!storage_.clearNetworks()) {
    server_.send(500, "text/plain", "Failed to clear saved networks");
    return;
  }

  server_.send(200, "text/html; charset=utf-8",
               "<html><body style='font-family:system-ui'><h2>Cleared</h2><p>Newo will reboot into setup mode.</p></body></html>");
  scheduleReboot();
}

void NewoPortal::handleNotFound() {
  if (wifi_.setupApActive()) {
    server_.sendHeader("Location", "/");
    server_.send(302, "text/plain", "Newo setup");
    return;
  }

  server_.send(404, "text/plain", "Not found");
}

void NewoPortal::scheduleReboot() {
  rebootScheduled_ = true;
  rebootAtMs_ = millis() + NewoConfig::PORTAL_REBOOT_DELAY_MS;
}
