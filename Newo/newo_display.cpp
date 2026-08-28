#include "newo_display.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/Org_01.h>
#include <SPI.h>
#include <cstring>

#include "newo_config.h"

namespace {
constexpr uint16_t kWidth = 240;
constexpr uint16_t kHeight = 240;
constexpr uint32_t kTemporaryMs = 7'000;
constexpr uint32_t kEcoPageMs = 5'000;
constexpr uint16_t kWhite = ST77XX_WHITE;

void formatUptime(char* out, size_t size, uint32_t ms) {
  const uint32_t seconds = ms / 1000;
  snprintf(out, size, "%luh %02lum", static_cast<unsigned long>(seconds / 3600),
           static_cast<unsigned long>((seconds / 60) % 60));
}
}  // namespace

NewoDisplay::NewoDisplay()
    : display_(NewoConfig::DISPLAY_CS_PIN, NewoConfig::DISPLAY_DC_PIN, NewoConfig::DISPLAY_RST_PIN) {}

void NewoDisplay::begin() {
  SPI.begin(NewoConfig::DISPLAY_SCK_PIN, -1, NewoConfig::DISPLAY_MOSI_PIN, NewoConfig::DISPLAY_CS_PIN);
  display_.init(kWidth, kHeight);
  display_.setRotation(3);
  display_.setTextColor(kWhite);
  display_.setTextWrap(false);
  render();
}

bool NewoDisplay::setMode(NewoDisplayMode mode, const char* text, bool temporary) {
  if (mode > NewoDisplayMode::ECO) return false;
  mode_ = mode;
  strncpy(text_, text ? text : "", sizeof(text_) - 1);
  text_[sizeof(text_) - 1] = '\0';
  if (!temporary && mode != NewoDisplayMode::ECO) {
    persistentMode_ = mode;
    strncpy(persistentText_, text_, sizeof(persistentText_) - 1);
    persistentText_[sizeof(persistentText_) - 1] = '\0';
  }
  temporary_ = temporary;
  restoreAtMs_ = temporary ? millis() + kTemporaryMs : 0;
  dirty_ = true;
  return true;
}

void NewoDisplay::toggleEco() {
  temporary_ = false;
  if (!ecoEnabled_) {
    ecoEnabled_ = true;
    mode_ = NewoDisplayMode::ECO;
    text_[0] = '\0';
    ecoPage_ = 0;
    nextEcoPageMs_ = millis() + kEcoPageMs;
  } else {
    ecoEnabled_ = false;
    mode_ = persistentMode_;
    strncpy(text_, persistentText_, sizeof(text_) - 1);
    text_[sizeof(text_) - 1] = '\0';
  }
  dirty_ = true;
}

void NewoDisplay::updateTelemetry(bool wifiConnected, int32_t rssi, bool cloudConnected, uint32_t uptimeMs,
                                  uint32_t freeHeap, uint32_t freePsram, const NewoLog::Stats& logs) {
  telemetry_ = {wifiConnected, rssi, cloudConnected, uptimeMs, freeHeap, freePsram, logs};
}

void NewoDisplay::loop() {
  const uint32_t now = millis();
  if (temporary_ && static_cast<int32_t>(now - restoreAtMs_) >= 0) {
    temporary_ = false;
    mode_ = ecoEnabled_ ? NewoDisplayMode::ECO : persistentMode_;
    if (!ecoEnabled_) {
      strncpy(text_, persistentText_, sizeof(text_) - 1);
      text_[sizeof(text_) - 1] = '\0';
    }
    dirty_ = true;
  }
  if (ecoEnabled_ && !temporary_ && static_cast<int32_t>(now - nextEcoPageMs_) >= 0) {
    nextEcoPageMs_ = now + kEcoPageMs;
    ecoPage_ = (ecoPage_ + 1) % 3;
    dirty_ = true;
  }
  if (dirty_) render();
}

const char* NewoDisplay::statusFor(NewoDisplayMode mode) {
  switch (mode) {
    case NewoDisplayMode::LISTENING: return "LISTENING";
    case NewoDisplayMode::THINKING: return "THINKING";
    case NewoDisplayMode::SPEAKING: return "SPEAKING";
    case NewoDisplayMode::ERROR: return "ERROR";
    default: return "";
  }
}

void NewoDisplay::render() {
  dirty_ = false;
  display_.fillScreen(ST77XX_BLACK);
  if (mode_ == NewoDisplayMode::ECO) return drawEco();
  if (mode_ == NewoDisplayMode::MESSAGE || temporary_) return drawTextPage(mode_ == NewoDisplayMode::MESSAGE ? "" : statusFor(mode_), text_, true);
  drawFace();
}

void NewoDisplay::drawFace() {
  int16_t leftX = 42, rightX = 137, y = 89, w = 61, h = 35;
  if (mode_ == NewoDisplayMode::LISTENING) { leftX = 35; rightX = 132; w = 73; h = 38; }
  if (mode_ == NewoDisplayMode::THINKING) y = 78;
  if (mode_ == NewoDisplayMode::ERROR) { y = 99; h = 18; }
  display_.fillRoundRect(leftX, y, w, h, h / 2, kWhite);
  display_.fillRoundRect(rightX, y, w, h, h / 2, kWhite);
  const char* status = statusFor(mode_);
  if (status[0]) drawCentered(status, 207);
}

void NewoDisplay::drawTextPage(const char* heading, const char* body, bool dense) {
  if (heading && heading[0]) drawCentered(heading, dense ? 20 : 42, dense);
  drawWrapped(body, heading && heading[0] ? (dense ? 38 : 76) : (dense ? 28 : 94), dense);
}

void NewoDisplay::drawEco() {
  char body[160];
  char up[20];
  formatUptime(up, sizeof(up), telemetry_.uptimeMs);
  if (ecoPage_ == 0) {
    snprintf(body, sizeof(body), "ONLINE  %s\n\nWiFi   %s\nRSSI   %ld dBm\nUp     %s\nFW     %s",
             telemetry_.cloud ? "YES" : "NO", telemetry_.wifi ? "OK" : "OFF",
             static_cast<long>(telemetry_.rssi), up, NewoConfig::FIRMWARE_VERSION);
    drawTextPage("NEWO", body, true);
  } else if (ecoPage_ == 1) {
    snprintf(body, sizeof(body), "Heap   %luK\nPSRAM  %luK\nWarn   %lu\nError  %lu",
             static_cast<unsigned long>(telemetry_.heap / 1024), static_cast<unsigned long>(telemetry_.psram / 1024),
             static_cast<unsigned long>(telemetry_.logs.warnings), static_cast<unsigned long>(telemetry_.logs.errors));
    drawTextPage("HEALTH", body, true);
  } else {
    snprintf(body, sizeof(body), "WiFi   %s\nCloud  %s\nWarn   %lu\nError  %lu",
             telemetry_.wifi ? "OK" : "OFF", telemetry_.cloud ? "OK" : "OFF",
             static_cast<unsigned long>(telemetry_.logs.warnings), static_cast<unsigned long>(telemetry_.logs.errors));
    drawTextPage("SERVICES", body, true);
  }
}

void NewoDisplay::drawCentered(const char* text, int16_t y, bool dense) {
  display_.setFont(dense ? &Org_01 : &FreeSans9pt7b);
  int16_t x1, y1; uint16_t w, h;
  display_.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display_.setCursor((kWidth - w) / 2, y);
  display_.print(text);
  display_.setFont(nullptr);
}

void NewoDisplay::drawWrapped(const char* text, int16_t firstY, bool dense) {
  constexpr int16_t kMargin = 10;
  const int16_t maxWidth = kWidth - (kMargin * 2);
  const int16_t lineHeight = dense ? 10 : 20;
  display_.setFont(dense ? &Org_01 : &FreeSans9pt7b);
  char line[97] = {};
  size_t length = 0;
  int16_t y = firstY;
  const char* cursor = text ? text : "";
  auto flush = [&]() {
    if (length == 0 || y > 230) return;
    line[length] = '\0';
    drawCentered(line, y, dense);
    display_.setFont(dense ? &Org_01 : &FreeSans9pt7b);
    y += lineHeight;
    length = 0;
  };
  while (*cursor && y <= 230) {
    if (*cursor == '\n') { flush(); ++cursor; continue; }
    while (*cursor == ' ') ++cursor;
    if (!*cursor) break;
    if (*cursor == '\n') { flush(); ++cursor; continue; }
    char word[97] = {};
    size_t wordLength = 0;
    while (cursor[wordLength] && cursor[wordLength] != ' ' && cursor[wordLength] != '\n' && wordLength < sizeof(word) - 1) {
      word[wordLength] = cursor[wordLength]; ++wordLength;
    }
    word[wordLength] = '\0';
    cursor += wordLength;
    char candidate[97] = {};
    snprintf(candidate, sizeof(candidate), "%s%s%s", line, length ? " " : "", word);
    int16_t x1, y1; uint16_t width, height;
    display_.getTextBounds(candidate, 0, 0, &x1, &y1, &width, &height);
    if (width <= maxWidth) { strncpy(line, candidate, sizeof(line) - 1); length = strlen(line); continue; }
    if (length) flush();
    // A single long word is split character-by-character to remain in bounds.
    for (size_t index = 0; index < wordLength && y <= 230; ++index) {
      line[length++] = word[index]; line[length] = '\0';
      display_.getTextBounds(line, 0, 0, &x1, &y1, &width, &height);
      if (width > maxWidth) { line[--length] = '\0'; flush(); line[length++] = word[index]; line[length] = '\0'; }
    }
  }
  flush();
  display_.setFont(nullptr);
}
