#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <usb/usb_host.h>

#include "newo_usb_host.h"

// Independent Arduino/virtual-COM discovery client under NewoUsbHost.
//
// It deliberately does not claim a serial interface yet: no Newo feature uses
// Arduino bytes in production today. Keeping discovery/ownership separate now
// means CDC ACM, CH34x, CP210x or FTDI transport can be added later without
// changing storage/audio or creating a second USB host stack.
class NewoUsbVcp {
 public:
  bool begin(NewoUsbHost& host);
  bool ready() const { return ready_.load() && device_ != nullptr; }
  uint8_t address() const { return address_; }
  uint16_t vid() const { return vid_; }
  uint16_t pid() const { return pid_; }

 private:
  static void clientTaskEntry(void* arg);
  static void clientEvent(const usb_host_client_event_msg_t* event, void* arg);
  static bool configLooksLikeCdc(const usb_config_desc_t* config);
  static bool knownUsbSerial(uint16_t vid, uint16_t pid);

  void clientTask();
  void handleConnected(uint8_t address);
  void handleDisconnected(usb_device_handle_t device);
  void cleanupGoneDevice();

  NewoUsbHost* host_ = nullptr;
  TaskHandle_t clientTask_ = nullptr;
  usb_host_client_handle_t client_ = nullptr;
  usb_device_handle_t device_ = nullptr;
  uint8_t address_ = 0;
  uint16_t vid_ = 0;
  uint16_t pid_ = 0;
  std::atomic<bool> ready_{false};
  std::atomic<bool> removed_{false};
};

extern NewoUsbVcp newoUsbVcp;
