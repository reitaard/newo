#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <usb/msc_host.h>
#include <usb/usb_host.h>

// Owns the one ESP32-S3 USB Host Library instance for all Newo USB features.
//
// Functional subsystems are clients of this manager:
//   - NewoUsbStorage: ESP-IDF MSC class driver
//   - NewoUsbAudio:   dedicated async USB Host client
//   - NewoUsbVcp:     dedicated async USB Host client for Arduino/CDC discovery
//
// No subsystem may call usb_host_install()/usb_host_uninstall() itself. This
// keeps hub/device lifetime and the S3 FIFO budget global and deterministic.
class NewoUsbHost {
 public:
  bool begin();
  bool ready() const { return ready_.load(); }

  bool registerClient(const usb_host_client_config_t& config,
                      usb_host_client_handle_t* handle,
                      const char* name);
  bool deregisterClient(usb_host_client_handle_t handle, const char* name);

  // MSC's Espressif class driver owns its internal USB Host client, but host
  // installation/lifetime still belongs here. Storage calls these wrappers
  // instead of installing a second host stack.
  bool installMscClient(const msc_host_driver_config_t& config);
  bool uninstallMscClient();

  uint32_t directClientCount() const { return directClients_.load(); }
  bool mscClientInstalled() const { return mscInstalled_.load(); }

 private:
  static void hostTaskEntry(void* arg);
  void hostTask();

  TaskHandle_t hostTask_ = nullptr;
  std::atomic<bool> ready_{false};
  std::atomic<bool> hostInstalled_{false};
  std::atomic<bool> mscInstalled_{false};
  std::atomic<uint32_t> directClients_{0};
};

extern NewoUsbHost newoUsbHost;
