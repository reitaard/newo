#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <usb/msc_host.h>
#include <usb/usb_host.h>

// Owns the one ESP32-S3 USB Host Library instance for all Newo USB features.
//
// Startup is intentionally two-phase:
//   1. begin() installs the physical host and freezes the global FIFO budget.
//   2. storage/audio/VCP register as independent clients.
//   3. start() begins host event processing and enumeration.
//
// Registering clients before enumeration prevents a power-on device already on
// the hub from racing past a client that has not registered yet.
class NewoUsbHost {
 public:
  bool begin();
  bool start();

  // ready(): host library is installed, so clients may register.
  bool ready() const { return hostInstalled_.load(); }
  bool running() const { return running_.load(); }

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
  std::atomic<bool> hostInstalled_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> mscInstalled_{false};
  std::atomic<uint32_t> directClients_{0};
};

extern NewoUsbHost newoUsbHost;
