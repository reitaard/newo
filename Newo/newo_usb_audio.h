#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <usb/usb_host.h>

#include "newo_usb_audio_descriptors.h"

// Production USB speaker path for the bench-validated Audiocular D07.
//
// The D07 is UAC2 and exposes PCM16 stereo playback on interface 1 alt 1.
// Newo's cloud speaker stream remains 24 kHz mono PCM16; this class converts
// each 24 kHz mono sample into two identical 48 kHz stereo frames, preserving
// full digital amplitude at 100% volume while matching the D07 clock exactly.
//
// USB client callbacks are serviced by NewoUsbStorage's monitor task. Control
// requests may therefore block in the speaker task without blocking the host
// event loop. Isochronous callbacks only recycle fixed transfer objects.
class NewoUsbAudio {
 public:
  static constexpr uint32_t kOutputRate = 48'000;
  static constexpr size_t kMono24SamplesPerBatch = 192;  // 8 ms at 24 kHz.

  bool begin(usb_host_client_handle_t client);
  bool connected(usb_device_handle_t device, uint8_t address);
  void disconnected(usb_device_handle_t device);
  void service();

  bool speakerReady() const {
    return ready_.load() && !removed_.load() && device_ != nullptr && candidate_.valid;
  }
  bool speakerPlaying() const { return playing_.load(); }

  // Claim the D07 PCM16 stereo alternate and set its UAC2 clock to 48 kHz.
  bool beginSpeakerPlayback();

  // Queue up to one 8 ms batch of already-volume-scaled 24 kHz mono PCM16.
  // Short final batches are padded with digital silence only after all valid
  // source samples, so Newo's original PCM byte accounting is unchanged.
  bool writeSpeakerMono24(const int16_t* samples, size_t sampleCount,
                          uint32_t timeoutMs = 100);

  // Drain all submitted USB packets, return the interface to alt 0 and release
  // it. Returns false if any transfer/packet/control error occurred.
  bool endSpeakerPlayback(uint32_t* drainMs = nullptr);

  uint32_t transferErrors() const { return transferErrors_.load(); }
  uint32_t packetErrors() const { return packetErrors_.load(); }
  uint32_t completedPackets() const { return completedPackets_.load(); }

 private:
  struct PlaybackCandidate {
    uint8_t iface = 0;
    uint8_t alt = 0;
    uint8_t endpoint = 0;
    uint8_t clockId = 0;
    uint16_t mps = 0;
    bool valid = false;
  };

  struct ControlWait {
    SemaphoreHandle_t done = nullptr;
    usb_transfer_status_t status = USB_TRANSFER_STATUS_ERROR;
    int actualBytes = 0;
  };

  static constexpr uint16_t kD07Vid = 0x3302;
  static constexpr uint16_t kD07Pid = 0x3395;
  static constexpr uint8_t kControlInterface = 0;
  static constexpr uint8_t kPacketsPerTransfer = 8;
  static constexpr uint8_t kTransferCount = 4;
  static constexpr size_t kPacketBytes = 192;   // 1 ms: 48 frames * stereo * 16-bit.
  static constexpr size_t kTransferBytes = kPacketBytes * kPacketsPerTransfer;

  static uint16_t le16(const uint8_t* value);
  static uint32_t le32(const uint8_t* value);
  static void putLe32(uint8_t* value, uint32_t data);
  static PlaybackCandidate findPlayback(const usb_config_desc_t* config);

  static void controlDone(usb_transfer_t* transfer);
  static void speakerTransferDone(usb_transfer_t* transfer);
  void onSpeakerTransferDone(usb_transfer_t* transfer);

  esp_err_t controlRequest(uint8_t requestType, uint8_t request, uint16_t value,
                           uint16_t index, void* data, uint16_t length);
  esp_err_t setClockRate(uint32_t rate);
  esp_err_t getClockRate(uint32_t* rate);
  esp_err_t setInterface(uint8_t alt);

  bool allocateTransfers();
  void releaseTransfers();
  bool waitForDrain(uint32_t timeoutMs);
  void resetTransportStats();

  usb_host_client_handle_t client_ = nullptr;
  usb_device_handle_t device_ = nullptr;
  uint8_t address_ = 0;
  PlaybackCandidate candidate_ = {};

  QueueHandle_t freeTransfers_ = nullptr;
  SemaphoreHandle_t drained_ = nullptr;
  usb_transfer_t* transfers_[kTransferCount] = {};

  std::atomic<bool> ready_{false};
  std::atomic<bool> removed_{false};
  std::atomic<bool> playing_{false};
  std::atomic<bool> interfaceClaimed_{false};
  std::atomic<uint32_t> activeTransfers_{0};
  std::atomic<uint32_t> transferErrors_{0};
  std::atomic<uint32_t> packetErrors_{0};
  std::atomic<uint32_t> completedPackets_{0};
  std::atomic<uint32_t> completedBytes_{0};
};

extern NewoUsbAudio newoUsbAudio;
