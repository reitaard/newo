#include "newo_portal.h"

#include <WiFi.h>

#include "newo_config.h"
#include "newo_log.h"

void NewoPortal::begin() {
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/wifi/save", HTTP_POST, [this]() { handleSave(); });
  server_.on("/generate_204", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_.on("/gen_204", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_.on("/hotspot-detect.html", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_.on("/connecttest.txt", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_.on("/ncsi.txt", HTTP_ANY, [this]() { handleCaptiveProbe(); });
  server_.onNotFound([this]() { handleCaptiveProbe(); });
}

void NewoPortal::loop() {
  if (wifi_.provisioningActive() && !servicesStarted_) {
    dns_.start(53, "*", wifi_.provisioningIp());
    server_.begin();
    servicesStarted_ = true;
    NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::PROV, "PORTAL_READY");
  }
  if (!wifi_.provisioningActive() && servicesStarted_) {
    dns_.stop();
    server_.stop();
    servicesStarted_ = false;
  }
  if (servicesStarted_) {
    dns_.processNextRequest();
    server_.handleClient();
  }
  if (rebootAtMs_ && static_cast<int32_t>(millis() - rebootAtMs_) >= 0) ESP.restart();
}

String NewoPortal::escape(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += value[i]; break;
    }
  }
  return out;
}

String NewoPortal::buildPage() {
  String html;
  html.reserve(8192);
  html += F("<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'><title>Newo Wi-Fi</title><style>body{font-family:system-ui;max-width:680px;margin:24px auto;padding:0 16px;background:#f5f5f5}.card{background:white;padding:16px;margin:12px 0;border-radius:12px}input,button{box-sizing:border-box;width:100%;padding:12px;margin:5px 0;border:1px solid #bbb;border-radius:8px}button{background:#111;color:white}.net{border-top:1px solid #eee;padding-top:10px;margin-top:10px}</style><h1>Newo Wi-Fi</h1><div class='card'><b>Nearby 2.4 GHz networks</b>");
  const int16_t count = WiFi.scanNetworks(false, false);
  if (count <= 0) html += F("<p>No networks found. Refresh to scan again.</p>");
  for (int16_t i = 0; i < count; ++i) {
    const String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    html += F("<form class='net' method='post' action='/wifi/save'><b>");
    html += escape(ssid);
    html += F("</b> &nbsp; "); html += String(WiFi.RSSI(i));
    html += F(" dBm<input type='hidden' name='ssid' value=\""); html += escape(ssid);
    html += F("\"><input type='password' name='password' maxlength='63' placeholder='Wi-Fi password'><button>Save and connect</button></form>");
  }
  WiFi.scanDelete();
  html += F("</div><div class='card'><b>Enter network manually</b><form method='post' action='/wifi/save'><input name='ssid' maxlength='32' required placeholder='Network name'><input type='password' name='password' maxlength='63' placeholder='Wi-Fi password'><button>Save and connect</button></form></div>");
  return html;
}

void NewoPortal::handleRoot() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/html; charset=utf-8", buildPage());
}

void NewoPortal::handleSave() {
  const String ssid = server_.arg("ssid");
  const String password = server_.arg("password");
  if (!storage_.addOrUpdateNetwork(ssid, password)) {
    server_.send(400, "text/plain", "Could not save that network.");
    return;
  }
  NewoLog::log(NewoLog::Level::INFO, NewoLog::Subsystem::PROV, "PROV_SAVED");
  server_.send(200, "text/html", "<h2>Saved</h2><p>Newo is restarting and will connect to this network.</p>");
  rebootAtMs_ = millis() + NewoConfig::PROVISIONING_REBOOT_DELAY_MS;
}

void NewoPortal::handleCaptiveProbe() {
  server_.sendHeader("Location", String("http://") + wifi_.provisioningIp().toString() + "/");
  server_.send(302, "text/plain", "Newo Wi-Fi setup");
}
