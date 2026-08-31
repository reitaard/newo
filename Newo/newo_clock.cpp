#include "newo_display.h"

#include <Fonts/FreeSans12pt7b.h>
#include <time.h>
#include <cstring>

namespace {
constexpr char kTimeZone[] = "ICT-7";  // UTC+7 (Cambodia); POSIX TZ sign is reversed.
constexpr int16_t kClockY = 214;
constexpr int16_t kClockClearX = 34;
constexpr int16_t kClockClearY = 190;
constexpr int16_t kClockClearW = 172;
constexpr int16_t kClockClearH = 38;
}

void NewoDisplay::updateClock() {
  static bool timeConfigured = false;
  static time_t lastMinute = -1;
  static NewoDisplayMode lastMode = NewoDisplayMode::ECO;
  static NewoFaceStyle lastStyle = NewoFaceStyle::NEUTRAL;

  if (!timeConfigured) {
    configTzTime(kTimeZone, "pool.ntp.org", "time.nist.gov");
    timeConfigured = true;
  }

  // Keep text pages and ECO untouched. The clock belongs only to the face view,
  // and yields the lower screen whenever a face response is being shown.
  const bool visible = !temporary_ && mode_ != NewoDisplayMode::ECO &&
                       mode_ != NewoDisplayMode::MESSAGE && text_[0] == '\0';
  if (!visible) {
    lastMinute = -1;  // Force a redraw as soon as the face view returns.
    lastMode = mode_;
    lastStyle = faceStyle_;
    return;
  }

  const time_t now = time(nullptr);
  if (now < 1'700'000'000) return;  // SNTP has not synchronized yet.

  const time_t minute = now / 60;
  if (minute == lastMinute && mode_ == lastMode && faceStyle_ == lastStyle) {
    return;
  }

  struct tm localTime = {};
  localtime_r(&now, &localTime);

  char clockText[20] = {};
  strftime(clockText, sizeof(clockText), "%d %b, %H:%M", &localTime);
  if (clockText[0] == '0') memmove(clockText, clockText + 1, strlen(clockText));

  display_.fillRect(kClockClearX, kClockClearY, kClockClearW, kClockClearH, ST77XX_BLACK);
  display_.setFont(&FreeSans12pt7b);
  display_.setTextColor(ST77XX_WHITE);
  int16_t x1, y1;
  uint16_t width, height;
  display_.getTextBounds(clockText, 0, kClockY, &x1, &y1, &width, &height);
  display_.setCursor((240 - static_cast<int16_t>(width)) / 2, kClockY);
  display_.print(clockText);
  display_.setFont(nullptr);

  lastMinute = minute;
  lastMode = mode_;
  lastStyle = faceStyle_;
}
