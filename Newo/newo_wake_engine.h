#pragma once

#include <Arduino.h>
#include <ESP_I2S.h>
#include <ESP_SR.h>

// Minimal detector boundary. Audio/session ownership stays in NewoAudio; a wake
// backend only owns the already-configured I2S input while the device is ARMED.
class NewoWakeEngine {
 public:
  virtual ~NewoWakeEngine() = default;
  virtual bool start(I2SClass& i2s, sr_cb callback) = 0;
  virtual void stop() = 0;
  virtual const char* modelName() const = 0;
};

// Proven rollback backend. Keep this until Alfred has passed physical testing.
class NewoEspSrWakeEngine final : public NewoWakeEngine {
 public:
  bool start(I2SClass& i2s, sr_cb callback) override;
  void stop() override;
  const char* modelName() const override { return "wn9_hiwalle_tts2"; }

 private:
  bool running_ = false;
};

// Standalone microWakeWord/TFLite backend. It deliberately keeps the existing
// sr_cb boundary so NewoAudio's OFF -> ARMED -> STREAMING state machine does not
// need to know which local detector is active.
class NewoMicroWakeWordEngine final : public NewoWakeEngine {
 public:
  bool start(I2SClass& i2s, sr_cb callback) override;
  void stop() override;
  const char* modelName() const override { return "alfred"; }

 private:
  static void taskEntry(void* context);
  void task();

  I2SClass* i2s_ = nullptr;
  sr_cb callback_ = nullptr;
  TaskHandle_t task_ = nullptr;
  volatile bool stopRequested_ = false;
  volatile bool running_ = false;
};
