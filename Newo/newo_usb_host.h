#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <usb/msc_host.h>
#include <usb/cdc_acm_host.h>
#include <usb/usb_host.h>

#include "newo_usb_host_limits.h"

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
  // ESP32-S3 DWC has 200 FIFO lines (4 bytes each). This is a host-wide
  // resource, not an audio setting: periodic OUT fits the D07's 384-byte
  // packet, non-periodic OUT remains available for MSC/control, and RX remains
  // large enough for the 208-byte USB-audio endpoint observed on the bench.
  static constexpr unsigned kRxFifoLines = NewoUsbHostLimits::kRxFifoLines;
  static constexpr unsigned kNptxFifoLines = NewoUsbHostLimits::kNptxFifoLines;
  static constexpr unsigned kPtxFifoLines = NewoUsbHostLimits::kPtxFifoLines;
  static constexpr unsigned kFifoLinesTotal = NewoUsbHostLimits::kFifoLinesTotal;
  static constexpr unsigned kMaxPeriodicOutBytes = NewoUsbHostLimits::kMaxPeriodicOutBytes;
  static constexpr unsigned kMaxNonPeriodicOutBytes = NewoUsbHostLimits::kMaxNonPeriodicOutBytes;
  static constexpr unsigned kMaxInPacketBytes = NewoUsbHostLimits::kMaxInPacketBytes;
  static constexpr unsigned kHostChannels = NewoUsbHostLimits::kHostChannels;
  static constexpr unsigned kEnumerationChannelReserve = NewoUsbHostLimits::kEnumerationChannelReserve;

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

  // Espressif class drivers own their internal clients, but NewoUsbHost owns
  // installation order/lifetime so no feature can create another host stack.
  bool installCdcClient(const cdc_acm_host_driver_config_t& config);
  bool uninstallCdcClient();

  uint32_t directClientCount() const { return directClients_.load(); }
  bool mscClientInstalled() const { return mscInstalled_.load(); }
  bool cdcClientInstalled() const { return cdcInstalled_.load(); }

 private:
  static void hostTaskEntry(void* arg);
  void hostTask();

  TaskHandle_t hostTask_ = nullptr;
  std::atomic<bool> hostInstalled_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> mscInstalled_{false};
  std::atomic<bool> cdcInstalled_{false};
  std::atomic<uint32_t> directClients_{0};
};

extern NewoUsbHost newoUsbHost;
