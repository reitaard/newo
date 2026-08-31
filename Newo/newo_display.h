#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "newo_log.h"

enum class NewoDisplayMode : uint8_t { IDLE, LISTENING, THINKING, SPEAKING, ERROR, MESSAGE, ECO };
enum class NewoFaceStyle : uint8_t { NEUTRAL, HAPPY, ANGRY, TIRED, CURIOUS, CONFUSED, LAUGH, SWEAT, CYCLOPS };

class NewoDisplay {
 public:
  NewoDisplay();
  void begin();
  void loop();
  void updateClock();
  bool setMode(NewoDisplayMode mode, const char* text = nullptr, bool temporary = false);
  bool setFaceStyle(NewoFaceStyle style);
  // Playback is an animation hint only; it never owns display content or timing.
  void setSpeakerActive(bool active);
  void toggleEco();
  bool ecoEnabled() const { return ecoEnabled_; }
  void updateTelemetry(bool wifiConnected, int32_t rssi, bool cloudConnected, uint32_t uptimeMs,
                       uint32_t freeHeap, uint32_t freePsram, const NewoLog::Stats& logs);

 private:
  void render();
  void drawFace();
  void drawFaceFrame(uint32_t now);
  void drawStateAnimation(uint32_t now);
  void drawFaceResponse();
  void updateGaze(uint32_t now);
  void resetFaceMotion(uint32_t now);
  void applyEyeExpression(int16_t leftX, int16_t rightX, int16_t y, int16_t leftW, int16_t rightW,
                          int16_t height);
  void blitMonoCanvasFast(GFXcanvas1& canvas, int16_t x, int16_t y, int16_t width, int16_t height);
  void recordFaceFrame(uint32_t elapsedUs);
  void drawTextPage(const char* heading, const char* body, bool info = false);
  void drawMessage();
  void drawEco();
  void drawCentered(const char* text, int16_t y);
  void drawWrapped(const char* text, int16_t firstY, bool info, bool centered = false, uint8_t maxLines = 0);
  static const char* statusFor(NewoDisplayMode mode);

  Adafruit_ST7789 display_;
  GFXcanvas1 eyeCanvas_{200, 82};
  GFXcanvas1 activityCanvas_{96, 23};
  uint16_t monoLineBuffer_[200] = {};  // One RGB565 scanline; no full-color framebuffer.
  NewoDisplayMode mode_ = NewoDisplayMode::IDLE;
  NewoDisplayMode persistentMode_ = NewoDisplayMode::IDLE;
  NewoFaceStyle faceStyle_ = NewoFaceStyle::NEUTRAL;
  char text_[97] = {};
  char persistentText_[97] = {};
  bool ecoEnabled_ = false;
  bool temporary_ = false;
  bool speakerActive_ = false;
  bool dirty_ = true;
  uint32_t restoreAtMs_ = 0;
  uint32_t nextEcoPageMs_ = 0;
  uint8_t ecoPage_ = 0;
  uint32_t nextFaceFrameMs_ = 0;
  uint32_t modeStartedMs_ = 0;
  enum class BlinkPhase : uint8_t { OPEN, HALF_CLOSED, CLOSED, HALF_OPEN };
  uint32_t nextBlinkMs_ = 0;
  BlinkPhase blinkPhase_ = BlinkPhase::OPEN;
  uint8_t blinkFramesRemaining_ = 0;
  bool verificationBlink_ = true;
  int16_t gazeX_ = 0;
  int16_t gazeY_ = 0;
  int16_t gazeTargetX_ = 0;
  int16_t gazeTargetY_ = 0;
  uint32_t nextGazeMs_ = 0;
  uint32_t frameMetricsStartedMs_ = 0;
  uint32_t frameMetricsTotalUs_ = 0;
  uint32_t frameMetricsWorstUs_ = 0;
  uint16_t frameMetricsCount_ = 0;
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
