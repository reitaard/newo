#pragma once

#include <Adafruit_ST7789.h>

#include "newo_log.h"

enum class NewoDisplayMode : uint8_t { IDLE, LISTENING, THINKING, SPEAKING, ERROR, MESSAGE, ECO };

class NewoDisplay {
 public:
  NewoDisplay();
  void begin();
  void loop();
  bool setMode(NewoDisplayMode mode, const char* text = nullptr, bool temporary = false);
  void toggleEco();
  bool ecoEnabled() const { return ecoEnabled_; }
  void updateTelemetry(bool wifiConnected, int32_t rssi, bool cloudConnected, uint32_t uptimeMs,
                       uint32_t freeHeap, uint32_t freePsram, const NewoLog::Stats& logs);

 private:
  void render();
  void drawFace();
  void drawTextPage(const char* heading, const char* body, bool dense = false);
  void drawEco();
  void drawCentered(const char* text, int16_t y, bool dense = false);
  void drawWrapped(const char* text, int16_t firstY, bool dense);
  static const char* statusFor(NewoDisplayMode mode);

  Adafruit_ST7789 display_;
  NewoDisplayMode mode_ = NewoDisplayMode::IDLE;
  NewoDisplayMode persistentMode_ = NewoDisplayMode::IDLE;
  char text_[97] = {};
  char persistentText_[97] = {};
  bool ecoEnabled_ = false;
  bool temporary_ = false;
  bool dirty_ = true;
  uint32_t restoreAtMs_ = 0;
  uint32_t nextEcoPageMs_ = 0;
  uint8_t ecoPage_ = 0;
  struct Telemetry {
    bool wifi = false;
    int32_t rssi = 0;
    bool cloud = false;
    uint32_t uptimeMs = 0;
    uint32_t heap = 0;
    uint32_t psram = 0;
    NewoLog::Stats logs = {};
  } telemetry_;
};
