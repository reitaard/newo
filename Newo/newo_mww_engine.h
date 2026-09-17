#pragma once

#include <Arduino.h>

#include "newo_wake_engine.h"

// Standalone microWakeWord/TFLM backend. It intentionally preserves the
// existing ESP-SR-shaped callback boundary so NewoAudio does not need to know
// which local detector is active.
class NewoMicroWakeEngine final : public NewoWakeEngine {
 public:
  bool start(I2SClass& i2s, sr_cb callback) override;
  void stop() override;
  const char* modelName() const override { return "alfred"; }

 private:
  struct Runtime;
  static void taskEntry(void* context);
  void task();
  bool createRuntime();
  void destroyRuntime();

  I2SClass* i2s_ = nullptr;
  sr_cb callback_ = nullptr;
  TaskHandle_t task_ = nullptr;
  Runtime* runtime_ = nullptr;
  volatile bool stopRequested_ = false;
  volatile bool taskFinished_ = true;
  bool running_ = false;
  bool wakeLatched_ = false;
};
