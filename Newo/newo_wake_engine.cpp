#include "newo_wake_engine.h"

bool NewoEspSrWakeEngine::start(I2SClass& i2s, sr_cb callback) {
  if (running_) return true;
  ESP_SR.onEvent(callback);
  // Empty commands keep ESP-SR in WakeNet-only mode. The phrase itself comes
  // from the model partition, never from this input-format string.
  running_ = ESP_SR.begin(i2s, nullptr, 0, SR_CHANNELS_STEREO, SR_MODE_WAKEWORD, "MN");
  return running_;
}

void NewoEspSrWakeEngine::stop() {
  if (!running_) return;
  ESP_SR.end();
  running_ = false;
}
