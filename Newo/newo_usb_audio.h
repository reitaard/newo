#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <usb/usb_host.h>

#include "newo_usb_audio_descriptors.h"
#include "newo_usb_host.h"

// Dedicated D07 UAC2 client of NewoUsbHost.
//
// This class owns its own USB Host client handle and event task. It can retain,
// claim and stream the D07 while storage and Arduino/VCP clients simultaneously
// own different devices/interfaces behind the same hub.
class NewoUsbAudio {
 public:
  static constexpr uint32_t kOutputRate = 48'000;
  static constexpr size_t kMono24SamplesPerBatch = 192;  // 8 ms at 24 kHz.

  bool begin(NewoUsbHost& host);

  bool speakerReady() const {
    return ready_.load() && !removed_.load() && device_ != nullptr && candidate_.valid;
  }
  bool speakerPlaying() const { return playing_.load(); }

  bool beginSpeakerPlayback();
  bool writeSpeakerMono24(const int16_t* samples, size_t sampleCount,
                          uint32_t timeoutMs = 100);
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

  enum class ControlState : uint8_t {
    WAITING = 0,
    COMPLETED = 1,
    ABANDONED = 2,
  };

  struct ControlWait {
    SemaphoreHandle_t done = nullptr;
    std::atomic<ControlState> state{ControlState::WAITING};
    usb_transfer_status_t status = USB_TRANSFER_STATUS_ERROR;
    int actualBytes = 0;
  };

  static constexpr uint16_t kD07Vid = 0x3302;
  static constexpr uint16_t kD07Pid = 0x3395;
  static constexpr uint8_t kControlInterface = 0;
  static constexpr uint8_t kPacketsPerTransfer = 8;
  static constexpr uint8_t kTransferCount = 4;
  static constexpr size_t kPacketBytes = 192;
  static constexpr size_t kTransferBytes = kPacketBytes * kPacketsPerTransfer;

  static uint16_t le16(const uint8_t* value);
  static uint32_t le32(const uint8_t* value);
  static void putLe32(uint8_t* value, uint32_t data);
  static PlaybackCandidate findPlayback(const usb_config_desc_t* config);

  static void clientTaskEntry(void* arg);
  static void clientEvent(const usb_host_client_event_msg_t* event, void* arg);
  void clientTask();
  void handleConnected(uint8_t address);
  void handleDisconnected(usb_device_handle_t device);
  void service();

  static void controlDone(usb_transfer_t* transfer);
  static void speakerTransferDone(usb_transfer_t* transfer);
  void onSpeakerTransferDone(usb_transfer_t* transfer);

  esp_err_t controlRequest(uint8_t requestType, uint8_t request, uint16_t value,
                           uint16_t index, void* data, uint16_t length);
  esp_err_t setClockRate(uint32_t rate);
  esp_err_t getClockRate(uint32_t* rate);
  esp_err_t setInterface(uint8_t alt);

  bool attachIfD07(usb_device_handle_t device, uint8_t address);
  bool allocateTransfers();
  void releaseTransfers();
  bool waitForDrain(uint32_t timeoutMs);
  void resetTransportStats();

  NewoUsbHost* host_ = nullptr;
  TaskHandle_t clientTask_ = nullptr;
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
