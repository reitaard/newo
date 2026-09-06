#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <usb/msc_host.h>
#include <usb/msc_host_vfs.h>

#include "newo_usb_host.h"

// Mass-storage client of NewoUsbHost. USB transport and filesystem/media have
// deliberately separate lifetimes: an enumerated controller can stay open while
// media is absent, and D07/VCP keep running while storage recovers.
class NewoUsbStorage {
 public:
  bool begin(NewoUsbHost& host);
  bool mounted() const { return mounted_; }

  // Changes every time /usb is mounted or invalidated. A future script loader
  // can snapshot this value while copying a script into RAM/PSRAM and refuse to
  // continue filesystem I/O if the generation changes underneath it.
  uint32_t generation() const { return mountGeneration_; }

 private:
  static void workerTaskEntry(void* arg);
  static void mscEvent(const msc_host_event_t* event, void* arg);

  void workerTask();
  void handleConnected(uint8_t address);
  void handleDisconnected(msc_host_device_handle_t device);
  void probeMediaAndMount(bool firstProbe);
  bool mountFilesystem();
  void invalidateFilesystem(const char* reason);
  void releaseDevice();

  NewoUsbHost* host_ = nullptr;
  TaskHandle_t workerTask_ = nullptr;
  portMUX_TYPE eventLock_ = portMUX_INITIALIZER_UNLOCKED;
  bool connectPending_ = false;
  uint8_t pendingAddress_ = 0;
  bool disconnectPending_ = false;
  msc_host_device_handle_t pendingDisconnectDevice_ = nullptr;

  msc_host_device_handle_t device_ = nullptr;
  msc_host_vfs_handle_t vfs_ = nullptr;
  volatile bool mounted_ = false;
  volatile uint32_t mountGeneration_ = 0;

  bool mediaWaiting_ = false;
  bool releaseRetryPending_ = false;
  uint32_t retryDelayMs_ = 0;
  uint8_t probeFailures_ = 0;
};
