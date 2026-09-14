#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ESP_I2S.h>
#include <ESP_SR.h>

#include "newo_config.h"
#include "newo_display.h"
#include "newo_voice_state.h"
#include "newo_wifi.h"
#include "newo_wake_engine.h"

// Voice owns I2S in exactly one place: ESP_SR while ARMED, or the temporary
// streaming task while STREAMING. OFF owns neither.
class NewoAudio {
 public:
  enum class MicMode : uint8_t { RAW = 0, NS = 1 };
  struct MicMetrics {
    uint32_t rawRms = 0, cleanRms = 0, rawPeak = 0, cleanPeak = 0;
    uint32_t rawClipped = 0, cleanClipped = 0, noiseFloorRms = 0;
  };
  NewoAudio(NewoWiFi& wifi, NewoDisplay& display);
  void begin();
  void loop();
  bool setEnabled(bool enabled);
  bool setMicProcessing(MicMode mode, uint8_t nsLevel);
  MicMode micMode() const { return micMode_; }
  uint8_t micNsLevel() const { return micNsLevel_; }
  const MicMetrics& micMetrics() const { return micMetrics_; }
  // Starts one direct microphone session, or cancels it when already streaming.
  // It intentionally does not enable or re-arm WakeNet.
  bool manualToggle();
  // Starts one direct session from a physical control. Unlike manualToggle(),
  // this never turns off or cancels an existing session.
  bool startPhysicalVoiceTrigger();
  // Temporarily releases WakeNet while preserving the user's OFF/ARMED choice.
  // Returns false only when an active STREAMING session makes playback unsafe.
  bool setPlaybackActive(bool active);
  // Release a deferred hands-free re-arm only after the server's complete
  // assistant/speaker turn reaches a terminal boundary.
  void completeAssistantTurn();
  bool wakeNetRearmPending() const { return awaitingAssistantCompletion_; }
  NewoVoiceState state() const { return state_; }
  uint32_t wakeCount() const { return wakeCount_; }
  uint32_t sessionCount() const { return sessionCount_; }
  bool voiceConnected() const { return voiceConnected_; }
  uint32_t failures() const { return failures_; }
  uint32_t timeouts() const { return timeouts_; }
  bool transitionPending() const { return transitionPending_; }

 private:
  static void srEvent(sr_event_t event, int commandId, int phraseId);
  static void streamTaskEntry(void* context);
  void streamTask();
  bool startWakeNet();
  void stopWakeNet();
  bool configureI2s();
  void releaseI2s();
  bool beginStreaming(bool rearmAfterStream);
  void finishStreaming(const char* reason);
  void handleVoiceEvent(WStype_t type, uint8_t* payload, size_t length);

  static NewoAudio* instance_;
  NewoWiFi& wifi_;
  NewoDisplay& display_;
  I2SClass i2s_;
  NewoEspSrWakeEngine wakeEngine_;
  WebSocketsClient voiceWebSocket_;
  TaskHandle_t streamTask_ = nullptr;
  volatile NewoVoiceState state_ = NewoVoiceState::OFF;
  volatile bool wakePending_ = false;
  volatile bool stopStreaming_ = false;
  volatile bool streamFinished_ = false;
  volatile bool voiceConnected_ = false;
  volatile bool enabled_ = NewoConfig::VOICE_DEFAULT_ENABLED;
  bool wakeNetRunning_ = false;
  bool i2sRunning_ = false;
  bool transitionPending_ = false;
  bool playbackSuppressed_ = false;
  bool rearmAfterStream_ = false;
  bool awaitingAssistantCompletion_ = false;
  bool assistantTerminalSeen_ = false;
  const char* volatile streamEndReason_ = nullptr;
  uint32_t streamStartedMs_ = 0;
  uint32_t wakeCount_ = 0;
  uint32_t sessionCount_ = 0;
  uint32_t failures_ = 0;
  uint32_t timeouts_ = 0;
  MicMode micMode_ = MicMode::NS;
  uint8_t micNsLevel_ = 1;
  MicMetrics micMetrics_;
};
