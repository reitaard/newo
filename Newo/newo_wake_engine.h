#pragma once

#include <ESP_I2S.h>
#include <ESP_SR.h>

// Minimal detector boundary. Audio/session ownership stays in NewoAudio; the
// active implementation is the standalone Alfred microWakeWord engine.
// sr_cb remains only as the existing callback ABI used by NewoAudio.
class NewoWakeEngine {
 public:
  virtual ~NewoWakeEngine() = default;
  virtual bool start(I2SClass& i2s, sr_cb callback) = 0;
  virtual void stop() = 0;
  virtual const char* modelName() const = 0;
};
