#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "newo_log.h"

enum class NewoDisplayMode : uint8_t { IDLE, LISTENING, THINKING, SPEAKING, ERROR, MESSAGE, ECO };
enum class NewoFaceStyle : uint8_t { NEUTRAL, HAPPY, ANGRY, TIRED, CURIOUS, CONFUSED, LAUGH, SWEAT, CYCLOPS, CLOSED, WINK_LEFT, WINK_RIGHT, LOOK_LEFT, LOOK_RIGHT, LOOK_UP, LOOK_DOWN, LOOK_UP_LEFT, LOOK_UP_RIGHT, LOOK_DOWN_LEFT, LOOK_DOWN_RIGHT, SURPRISED, SLEEPY };

class NewoDisplay {
 public:
  NewoDisplay();
  void begin();
  void loop();
  void updateClock();
  bool setMode(NewoDisplayMode mode, const char* text = nullptr, bool temporary = false);
  bool setFaceStyle(NewoFaceStyle style);
  // Runtime signals are arbitrated independently of the persistent display mode.
  void setListeningActive(bool active);
  void setAssistantThinking(bool active);
  void setSpeakerActive(bool active);
  void noteSystemError();
  void toggleEco();
  bool ecoEnabled() const { return ecoEnabled_; }
  void setClockEnabled(bool enabled);
  bool clockEnabled() const { return clockEnabled_; }
  void updateTelemetry(bool wifiConnected, int32_t rssi, bool cloudConnected, uint32_t uptimeMs,
                       uint32_t freeHeap, uint32_t freePsram, const NewoLog::Stats& logs);

 private:
  void render();
  void drawFace();
  void drawFaceFrame(uint32_t now);
  void drawStateAnimation(uint32_t now);
  void drawFaceResponse();
  void updateGaze(uint32_t now);
  void updateAutonomousIdleGaze(uint32_t now);
  void chooseAutonomousGazeTarget();
  void initializeAutonomousState(uint32_t now);
  void updateAutonomousState(uint32_t now);
  void resetAutonomousEpisode(uint32_t now);
  void updateAutonomousEpisode(uint32_t now);
  void scheduleNextAutonomousEpisode(uint32_t now);
  void chooseAutonomousEpisode(uint32_t now);
  void finishAutonomousEpisode(uint32_t now);
  void beginAutonomousEpisodeGaze(uint32_t now, int16_t targetX, int16_t targetY, uint16_t holdMs);
  void advanceAutonomousEpisode(uint32_t now);
  void noteInteraction(uint32_t now, uint8_t energyGain, uint8_t curiosityGain, uint8_t socialGain);
  void noteError();
  uint32_t adjustAutonomousFixation(uint32_t fixationMs) const;
  bool autonomousIdle() const;
  void scheduleNextBilateralBlink(uint32_t now);
  void startBilateralBlink(bool allowAutonomousVariation, uint8_t forcedBlink = 0);
  void queuePostSaccadeBlink(uint32_t now);
  void resetFaceMotion(uint32_t now);
  void syncEffectiveMode(uint32_t now);
  NewoDisplayMode effectiveMode(uint32_t now) const;
  void applyEyeExpression(int16_t leftX, int16_t rightX, int16_t y, int16_t leftW, int16_t rightW,
                          int16_t height, NewoDisplayMode mode);
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
  bool autoFaceEnabled_ = true;
  NewoDisplayMode lastEffectiveMode_ = NewoDisplayMode::IDLE;
  bool lastEffectiveAutoFace_ = false;
  char text_[97] = {};
  char persistentText_[97] = {};
  bool ecoEnabled_ = false;
  bool clockEnabled_ = true;
  bool temporary_ = false;
  bool speakerActive_ = false;
  bool dirty_ = true;
  uint32_t restoreAtMs_ = 0;
  uint32_t nextEcoPageMs_ = 0;
  uint8_t ecoPage_ = 0;
  uint32_t nextFaceFrameMs_ = 0;
  uint32_t modeStartedMs_ = 0;
  bool listeningActive_ = false;
  bool assistantThinking_ = false;
  bool errorActive_ = false;
  uint32_t errorUntilMs_ = 0;
  enum class BlinkPhase : uint8_t { OPEN, HALF_CLOSED, CLOSED, HALF_OPEN };
  enum class BlinkSchedulerState : uint8_t { WAITING, DOUBLE_PAUSE, DOUBLE_SECOND };
  enum class AutonomousGazePhase : uint8_t { CHOOSE_TARGET, MOVING, FIXATING, MICRO_CORRECTION };
  enum class AutonomousEpisode : uint8_t { WAITING, CURIOUS_SCAN, LOW_ENERGY, SOCIAL_ATTENTION, ALERT_CHECK };
  enum class InactivityStage : uint8_t { ACTIVE, RELAXED, DROWSY };
  const char* contextName(NewoDisplayMode mode) const;
  static const char* episodeName(AutonomousEpisode episode);
  void recordGazeTarget(uint16_t holdMs);
  void maybeLogEyeStats(uint32_t now);
  uint32_t nextBlinkMs_ = 0;
  BlinkPhase blinkPhase_ = BlinkPhase::OPEN;
  BlinkSchedulerState blinkSchedulerState_ = BlinkSchedulerState::WAITING;
  uint8_t blinkFramesRemaining_ = 0;
  bool longBlink_ = false;
  bool postSaccadeBlinkPending_ = false;
  uint32_t nextWinkMs_ = 0;
  uint32_t winkStartedMs_ = 0;
  bool winkActive_ = false;
  bool winkLeft_ = false;
  AutonomousGazePhase autonomousGazePhase_ = AutonomousGazePhase::CHOOSE_TARGET;
  AutonomousEpisode autonomousEpisode_ = AutonomousEpisode::WAITING;
  uint32_t nextAutonomousEpisodeMs_ = 0;
  uint16_t autonomousEpisodeHoldMs_ = 0;
  uint8_t autonomousEpisodeStep_ = 0;
  int8_t autonomousEpisodeDirection_ = 1;
  bool autonomousEpisodeBlinkRequested_ = false;
  bool autonomousEpisodeBlinkStarted_ = false;
  uint32_t fixationUntilMs_ = 0;
  uint32_t microCorrectionAtMs_ = 0;
  bool microCorrectionPending_ = false;
  bool autonomousGazeLargeShift_ = false;
  uint8_t energy_ = 70;
  uint8_t curiosity_ = 42;
  uint8_t social_ = 38;
  uint8_t stress_ = 5;
  uint8_t idleDriftTicks_ = 0;
  uint32_t nextAutonomousStateMs_ = 0;
  uint32_t lastInteractionMs_ = 0;
  uint32_t nextAutonomousStateLogMs_ = 0;
  uint32_t eyeContextChanges_ = 0;
  uint32_t eyeGazeEvents_ = 0;
  uint32_t eyeMeaningfulGazeEvents_ = 0;
  uint32_t eyeBlinkEvents_ = 0;
  uint32_t eyeDoubleBlinkEvents_ = 0;
  uint32_t eyeLongBlinkEvents_ = 0;
  uint32_t eyeEpisodeStarts_ = 0;
  uint32_t eyeEpisodeCompletions_ = 0;
  uint32_t eyeErrorEvents_ = 0;
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
