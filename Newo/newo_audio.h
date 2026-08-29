#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ESP_I2S.h>
#include <ESP_SR.h>

#include "newo_config.h"
#include "newo_display.h"
#include "newo_voice_state.h"
#include "newo_wifi.h"

// Voice owns I2S in exactly one place: ESP_SR while ARMED, or the temporary
// streaming task while STREAMING. OFF owns neither.
class NewoAudio {
 public:
  NewoAudio(NewoWiFi& wifi, NewoDisplay& display);
  void begin();
  void loop();
  bool setEnabled(bool enabled);
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
  void beginStreaming();
  void finishStreaming(const char* reason);
  void handleVoiceEvent(WStype_t type, uint8_t* payload, size_t length);

  static NewoAudio* instance_;
  NewoWiFi& wifi_;
  NewoDisplay& display_;
  I2SClass i2s_;
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
  const char* volatile streamEndReason_ = nullptr;
  uint32_t streamStartedMs_ = 0;
  uint32_t wakeCount_ = 0;
  uint32_t sessionCount_ = 0;
  uint32_t failures_ = 0;
  uint32_t timeouts_ = 0;
};
