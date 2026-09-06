#pragma once
#include <Arduino.h>
#include <usb/usb_host.h>
#include <usb/uac_host.h>
#include <atomic>
#include "newo_usb_audio_descriptors.h"

// Lives on the existing USB monitor worker. The official driver has its own
// event task, so blocking control requests never stall the shared host daemon.
class NewoUsbAudio {
 public:
  bool begin(usb_host_client_handle_t client);
  bool connected(usb_device_handle_t device, uint8_t address);
  void disconnected(usb_device_handle_t device);
  void service();
 private:
  struct Stream {
    uac_host_device_handle_t handle = nullptr;
    std::atomic<bool> gone{false};
    std::atomic<uint32_t> errors{0};
    NewoUac::Alt alt;
    uint32_t rate = 0, started = 0, reported = 0, bytes = 0, samples = 0;
    uint32_t lastBytes = 0, toneFrames = 0, writeFailures = 0, activeUnderruns = 0;
    uint64_t squares = 0;
    unsigned peak = 0;
    bool active = false, passed = false, closing = false;
  };
  static void deviceEvent(uac_host_device_handle_t, uac_host_device_event_t event, void* arg);
  void command(const char* cmd);
  bool start(Stream& stream, bool mic);
  bool stop(Stream& stream);
  void report(Stream& stream, bool mic);
  void pumpMic();
  void pumpTone();
  usb_host_client_handle_t client_ = nullptr;
  usb_device_handle_t device_ = nullptr;
  uint8_t address_ = 0;
  bool enabled_ = false, removed_ = false, duplex_ = false;
  NewoUac::DescriptorSet descriptors_;
  Stream mic_, spk_;
  char command_[32] = {};
  unsigned commandLength_ = 0;
  bool commandOverflow_ = false;
  int16_t pcm_[960] = {};  // 10ms at 48k stereo, reused by RX/TX.
};
extern NewoUsbAudio newoUsbAudio;
