#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <usb/msc_host.h>
#include <usb/msc_host_vfs.h>

class NewoUsbStorage {
 public:
  // Starts the native ESP32-S3 USB-OTG host service. GPIO19 (D-) and GPIO20
  // (D+) are configured by the IDF USB Host Library; they are not GPIOs here.
  bool begin();
  bool mounted() const { return mounted_; }

 private:
  static void hostTaskEntry(void* arg);
  static void monitorTaskEntry(void* arg);
  static void workerTaskEntry(void* arg);
  static void monitorClientEvent(const usb_host_client_event_msg_t* event, void* arg);
  static void mscEvent(const msc_host_event_t* event, void* arg);

  void hostTask();
  void monitorTask();
  void workerTask();
  void handleConnected(uint8_t address);
  void handleDisconnected(msc_host_device_handle_t device);
  void logDevice(uint8_t address);
  void releaseMountedDevice();

  TaskHandle_t workerTask_ = nullptr;
  portMUX_TYPE eventLock_ = portMUX_INITIALIZER_UNLOCKED;
  bool connectPending_ = false;
  uint8_t pendingAddress_ = 0;
  bool disconnectPending_ = false;
  msc_host_device_handle_t pendingDisconnectDevice_ = nullptr;
  usb_host_client_handle_t monitorClient_ = nullptr;
  msc_host_device_handle_t device_ = nullptr;
  msc_host_vfs_handle_t vfs_ = nullptr;
  volatile bool mounted_ = false;
  bool hostInstalled_ = false;
  bool mscInstalled_ = false;

  static NewoUsbStorage* instance_;
};
