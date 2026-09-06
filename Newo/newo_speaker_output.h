#pragma once

#include <Arduino.h>
#include <ESP_I2S.h>

#include "newo_usb_audio.h"

// Keeps the existing, fully validated Newo speaker/WebSocket/Opus pipeline
// untouched while choosing the physical sink at playback start.
//
// D07 present  -> USB UAC2 is the audible sink. The I2S peripheral runs silent
//                 only as the existing DMA-drain timing source.
// D07 absent   -> existing MAX98357A I2S output remains the fallback.
class NewoSpeakerOutput {
 public:
  void setPins(int8_t bclk, int8_t ws, int8_t dout, int8_t din = -1, int8_t mclk = -1) {
    physical_.setPins(bclk, ws, dout, din, mclk);
  }

  bool begin(i2s_mode_t mode, uint32_t rate, i2s_data_bit_width_t bits,
             i2s_slot_mode_t channels, int8_t slotMask = -1,
             i2s_role_t role = I2S_ROLE_MASTER);
  bool end();
  size_t write(const uint8_t* buffer, size_t size);
  i2s_chan_handle_t txChan() { return physical_.txChan(); }

  bool usbActive() const { return usbActive_; }
  bool usbHealthy() const { return usbHealthy_; }

 private:
  bool flushUsbStage(bool finalBatch);

  I2SClass physical_;
  bool usbActive_ = false;
  bool usbHealthy_ = true;
  int16_t usbStage_[NewoUsbAudio::kMono24SamplesPerBatch] = {};
  size_t usbStageSamples_ = 0;
  uint8_t silentI2s_[1024] = {};
};
