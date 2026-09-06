#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <usb/msc_host.h>
#include <usb/msc_host_vfs.h>

#include "newo_usb_host.h"

// Mass-storage client of NewoUsbHost. It never installs or services the USB
// Host Library itself; the shared manager owns that lifetime for every device
// behind the hub.
class NewoUsbStorage {
 public:
  bool begin(NewoUsbHost& host);
  bool mounted() const { return mounted_; }

 private:
  static void workerTaskEntry(void* arg);
  static void mscEvent(const msc_host_event_t* event, void* arg);

  void workerTask();
  void handleConnected(uint8_t address);
  void handleDisconnected(msc_host_device_handle_t device);
  void releaseMountedDevice();

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

  // A card reader (or a thumb-drive controller during recovery) can enumerate
  // correctly while reporting SCSI NOT READY / MEDIUM NOT PRESENT. Keep the
  // USB host and other class clients alive and reprobe only the MSC address.
  bool mediaRetryPending_ = false;
  uint8_t mediaRetryAddress_ = 0;
  bool mediaWaiting_ = false;
};
