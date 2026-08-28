#include "newo_display.h"

#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <SPI.h>
#include <cmath>
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
  if (mode_ != mode) modeStartedMs_ = millis();
  mode_ = mode;
  strncpy(text_, text ? text : "", sizeof(text_) - 1);
  text_[sizeof(text_) - 1] = '\0';
  if (!temporary && mode != NewoDisplayMode::ECO) {
    // A manual /newo selection supersedes ECO; transient command-result pages do not.
    ecoEnabled_ = false;
    ecoPage_ = 0;
    nextEcoPageMs_ = 0;
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
  if (!temporary_ && mode_ != NewoDisplayMode::ECO && mode_ != NewoDisplayMode::MESSAGE &&
      static_cast<int32_t>(now - nextFaceFrameMs_) >= 0) {
    nextFaceFrameMs_ = now + 50;  // 20 FPS, deliberately below audio/control loop rates.
    drawFaceFrame(now);
  }
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
  if (temporary_) return drawTextPage("", text_, true);
  if (mode_ == NewoDisplayMode::MESSAGE) return drawMessage();
  drawFace();
  drawFaceResponse();
}

void NewoDisplay::drawFace() {
  const char* status = statusFor(mode_);
  if (status[0]) drawCentered(status, 145);
  // Give each newly-entered face state one visible verification blink, then use 3–7 s intervals.
  nextBlinkMs_ = millis() + 1'000;
  nextFaceFrameMs_ = 0;
  drawFaceFrame(millis());
}

void NewoDisplay::drawFaceFrame(uint32_t now) {
  // Compose the whole 200x82 eye region off-screen, then blit a complete frame.
  // This prevents the black clear/draw tear visible with direct TFT rendering.
  eyeCanvas_.fillScreen(0);
  if (!blinking_ && static_cast<int32_t>(now - nextBlinkMs_) >= 0) {
    blinking_ = true;
    blinkStartedMs_ = now;
  }
  constexpr uint32_t kBlinkDurationMs = 170;
  if (blinking_ && now - blinkStartedMs_ >= kBlinkDurationMs) {
    blinking_ = false;
    nextBlinkMs_ = now + random(3'000, 7'001);
  }
  const float phase = static_cast<float>(now % 3000) / 3000.0f * 6.2831853f;
  int16_t floatY = static_cast<int16_t>(sinf(phase) * 1.0f);
  int16_t gazeX = 0;
  if (mode_ == NewoDisplayMode::IDLE) gazeX = static_cast<int16_t>(sinf(phase * 0.55f) * 3.0f);
  if (mode_ == NewoDisplayMode::THINKING) gazeX = static_cast<int16_t>(sinf(phase * 0.32f) * 5.0f);
  if (mode_ == NewoDisplayMode::SPEAKING) gazeX = static_cast<int16_t>(sinf(phase * 1.5f) * 2.0f);
  int16_t shake = 0;
  if (mode_ == NewoDisplayMode::ERROR && now - modeStartedMs_ < 420) {
    shake = static_cast<int16_t>(sinf(static_cast<float>(now - modeStartedMs_) * 0.06f) * 3.0f);
  }
  int16_t leftX = 42 + gazeX + shake, rightX = 137 + gazeX + shake, y = 66 + floatY, w = 61, h = 35;
  if (mode_ == NewoDisplayMode::LISTENING) { leftX = 35 + gazeX; rightX = 132 + gazeX; w = 73; h = 38; }
  if (mode_ == NewoDisplayMode::THINKING) y -= 5;
  if (mode_ == NewoDisplayMode::ERROR) { y += 10; h = 18; }
  const int16_t baseHeight = h;
  if (blinking_) {
    const float progress = static_cast<float>(now - blinkStartedMs_) / kBlinkDurationMs;
    // Quadratic easing makes the 170 ms blink visibly close for the sampled 20 FPS frames.
    const float edge = progress < 0.5f ? 1.0f - progress * 2.0f : progress * 2.0f - 1.0f;
    const float openness = edge * edge;
    h = static_cast<int16_t>(baseHeight * openness);
    if (h < 2) h = 2;
  }
  const int16_t localY = y - 40 + (baseHeight - h) / 2;
  eyeCanvas_.fillRoundRect(leftX - 20, localY, w, h, h / 2, 1);
  eyeCanvas_.fillRoundRect(rightX - 20, localY, w, h, h / 2, 1);
  display_.drawBitmap(20, 40, eyeCanvas_.getBuffer(), 200, 82, kWhite, ST77XX_BLACK);
  drawStateAnimation(now);
}

void NewoDisplay::drawStateAnimation(uint32_t now) {
  activityCanvas_.fillScreen(0);
  const float phase = static_cast<float>(now % 2400) / 2400.0f * 6.2831853f;
  if (mode_ == NewoDisplayMode::IDLE) {
    const int16_t width = 20 + static_cast<int16_t>((sinf(phase) + 1.0f) * 13.0f);
    activityCanvas_.drawFastHLine(48 - width, 12, width * 2, 1);
  } else if (mode_ == NewoDisplayMode::LISTENING) {
    for (int8_t i = 0; i < 7; ++i) {
      const int16_t height = 5 + static_cast<int16_t>((sinf(phase * 2.0f + i * 0.8f) + 1.0f) * 5.5f);
      activityCanvas_.fillRect(12 + i * 12, 16 - height, 5, height, 1);
    }
  } else if (mode_ == NewoDisplayMode::THINKING) {
    for (int8_t i = 0; i < 3; ++i) {
      const int16_t rise = static_cast<int16_t>((sinf(phase * 1.5f + i * 1.4f) + 1.0f) * 3.0f);
      activityCanvas_.fillCircle(36 + i * 12, 15 - rise, 2, 1);
    }
  } else if (mode_ == NewoDisplayMode::SPEAKING) {
    int16_t lastY = 172;
    for (int16_t x = 0; x <= 72; x += 4) {
      const int16_t y = 172 + static_cast<int16_t>(sinf(phase * 2.0f + x * 0.16f) * 6.0f);
      activityCanvas_.drawLine(x - 4, lastY - 160, x, y - 160, 1);
      lastY = y;
    }
  } else if (mode_ == NewoDisplayMode::ERROR) {
    activityCanvas_.setFont(&FreeSans9pt7b); activityCanvas_.setCursor(44, 17); activityCanvas_.print("!"); activityCanvas_.setFont(nullptr);
  }
  display_.drawBitmap(72, 160, activityCanvas_.getBuffer(), 96, 23, kWhite, ST77XX_BLACK);
}

void NewoDisplay::drawFaceResponse() {
  if (!text_[0]) return;
  // State responses are deliberately bounded to two left-aligned lines.
  char response[49] = {};
  strncpy(response, text_, sizeof(response) - 1);
  if (strlen(text_) >= sizeof(response)) { response[44] = '.'; response[45] = '.'; response[46] = '.'; response[47] = '\0'; }
  drawWrapped(response, 204, false, false, 2);
}

void NewoDisplay::drawTextPage(const char* heading, const char* body, bool info) {
  constexpr int16_t kMargin = 16;
  char inferredHeading[25] = {};
  if (info && (!heading || !heading[0]) && body) {
    const char* newline = strchr(body, '\n');
    const size_t length = newline ? static_cast<size_t>(newline - body) : strlen(body);
    if (length > 0 && length < sizeof(inferredHeading)) {
      memcpy(inferredHeading, body, length);
      heading = inferredHeading;
      body = newline ? newline + 1 : "";
    }
  }
  int16_t firstY = 28;
  if (heading && heading[0]) {
    display_.setFont(&FreeSansBold9pt7b);
    display_.setCursor(kMargin, 24);
    display_.print(heading);
    display_.setFont(nullptr);
    firstY = 48;
  }
  drawWrapped(body, firstY, info, false);
}

void NewoDisplay::drawMessage() {
  uint8_t words = 0;
  bool inWord = false;
  for (const char* cursor = text_; *cursor; ++cursor) {
    if (*cursor == '\n') { words = 3; break; }
    if (*cursor != ' ' && !inWord) { ++words; inWord = true; }
    if (*cursor == ' ') inWord = false;
  }
  const bool shortMessage = words > 0 && words <= 2 && strlen(text_) <= 14;
  if (shortMessage) {
    display_.setFont(&FreeSans18pt7b);
    int16_t x1, y1; uint16_t w, h;
    display_.getTextBounds(text_, 0, 0, &x1, &y1, &w, &h);
    display_.setCursor((kWidth - w) / 2, 132);
    display_.print(text_);
    display_.setFont(nullptr);
    return;
  }
  drawTextPage("", text_, false);
}

void NewoDisplay::drawEco() {
  char body[160];
  char up[20];
  formatUptime(up, sizeof(up), telemetry_.uptimeMs);
  if (ecoPage_ == 0) {
    snprintf(body, sizeof(body), "Online  %s\nWiFi    %s\nRSSI    %ld dBm\nUp      %s\nFW      %s",
             telemetry_.cloud ? "YES" : "NO", telemetry_.wifi ? "OK" : "OFF",
             static_cast<long>(telemetry_.rssi), up, NewoConfig::FIRMWARE_VERSION);
    drawTextPage("NEWO", body, true);
  } else if (ecoPage_ == 1) {
    snprintf(body, sizeof(body), "Heap    %luK\nPSRAM   %luK\nWarn    %lu\nError   %lu",
             static_cast<unsigned long>(telemetry_.heap / 1024), static_cast<unsigned long>(telemetry_.psram / 1024),
             static_cast<unsigned long>(telemetry_.logs.warnings), static_cast<unsigned long>(telemetry_.logs.errors));
    drawTextPage("HEALTH", body, true);
  } else {
    snprintf(body, sizeof(body), "WiFi    %s\nCloud   %s\nWarn    %lu\nError   %lu",
             telemetry_.wifi ? "OK" : "OFF", telemetry_.cloud ? "OK" : "OFF",
             static_cast<unsigned long>(telemetry_.logs.warnings), static_cast<unsigned long>(telemetry_.logs.errors));
    drawTextPage("SERVICES", body, true);
  }
}

void NewoDisplay::drawCentered(const char* text, int16_t y) {
  display_.setFont(&FreeSans9pt7b);
  int16_t x1, y1; uint16_t w, h;
  display_.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display_.setCursor((kWidth - w) / 2, y);
  display_.print(text);
  display_.setFont(nullptr);
}

void NewoDisplay::drawWrapped(const char* text, int16_t firstY, bool info, bool centered, uint8_t maxLines) {
  constexpr int16_t kMargin = 16;
  const int16_t maxWidth = kWidth - (kMargin * 2);
  const int16_t lineHeight = info ? 17 : 19;
  display_.setFont(info ? &FreeMono9pt7b : &FreeSans9pt7b);
  char line[97] = {};
  size_t length = 0;
  int16_t y = firstY;
  uint8_t lines = 0;
  const char* cursor = text ? text : "";
  auto flush = [&]() {
    if (length == 0 || y > 230) return;
    line[length] = '\0';
    if (centered) drawCentered(line, y);
    else { display_.setCursor(kMargin, y); display_.print(line); }
    display_.setFont(info ? &FreeMono9pt7b : &FreeSans9pt7b);
    y += lineHeight;
    ++lines;
    length = 0;
  };
  while (*cursor && y <= 230 && (!maxLines || lines < maxLines)) {
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
