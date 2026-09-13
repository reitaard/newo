#pragma once

#include <ESP_I2S.h>
#include <ESP_SR.h>

// Minimal detector boundary. Audio/session ownership stays in NewoAudio; this
// backend only starts and stops the local wake detector on an owned I2S input.
class NewoWakeEngine {
 public:
  virtual ~NewoWakeEngine() = default;
  virtual bool start(I2SClass& i2s, sr_cb callback) = 0;
  virtual void stop() = 0;
  virtual const char* modelName() const = 0;
};

class NewoEspSrWakeEngine final : public NewoWakeEngine {
 public:
  bool start(I2SClass& i2s, sr_cb callback) override;
  void stop() override;
  const char* modelName() const override { return "wn9_hiwalle_tts2"; }

 private:
  bool running_ = false;
};
