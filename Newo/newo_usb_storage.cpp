#include "newo_usb_storage.h"

#include <inttypes.h>

#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <freertos/task.h>

namespace {
constexpr UBaseType_t kMscTaskPriority = 2;
constexpr UBaseType_t kWorkerTaskPriority = 1;
constexpr uint32_t kMscTaskStack = 4096;
constexpr uint32_t kWorkerTaskStack = 6144;
constexpr uint32_t kMediaRetryMs = 2000;

void logError(const char* event, esp_err_t error) {
  Serial.printf("[usb-storage] %s — reason=%s\n", event, esp_err_to_name(error));
}
}  // namespace

bool NewoUsbStorage::begin(NewoUsbHost& host) {
  if (host_ != nullptr) return host_ == &host;
  if (!host.ready()) {
    Serial.println("[usb-storage] CLIENT_FAILED — reason=host_not_ready");
    return false;
  }

  host_ = &host;
  if (xTaskCreate(workerTaskEntry, "newo-usb-vfs", kWorkerTaskStack, this,
                  kWorkerTaskPriority, &workerTask_) != pdPASS) {
    Serial.println("[usb-storage] CLIENT_FAILED — reason=worker_task");
    host_ = nullptr;
    workerTask_ = nullptr;
    return false;
  }

  msc_host_driver_config_t mscConfig = {};
  mscConfig.create_backround_task = true;
  mscConfig.task_priority = kMscTaskPriority;
  mscConfig.stack_size = kMscTaskStack;
  mscConfig.core_id = tskNO_AFFINITY;
  mscConfig.callback = mscEvent;
  mscConfig.callback_arg = this;
  if (!host.installMscClient(mscConfig)) {
    Serial.println("[usb-storage] CLIENT_FAILED — reason=msc_install");
    vTaskDelete(workerTask_);
    workerTask_ = nullptr;
    host_ = nullptr;
    return false;
  }

  Serial.println("[usb-storage] CLIENT_READY — mount=/usb");
  return true;
}

void NewoUsbStorage::workerTaskEntry(void* arg) {
  static_cast<NewoUsbStorage*>(arg)->workerTask();
}

void NewoUsbStorage::workerTask() {
  while (true) {
    // MEDIA NOT PRESENT does not generate another USB connect event when a card
    // is later inserted into an already-enumerated reader. Wake periodically
    // only while that state is active; otherwise this worker sleeps forever.
    const bool retryArmed = mediaRetryPending_;
    const TickType_t waitTicks = retryArmed ? pdMS_TO_TICKS(kMediaRetryMs) : portMAX_DELAY;
    const uint32_t notifications = ulTaskNotifyTake(pdTRUE, waitTicks);
    const bool retryDue = retryArmed && notifications == 0;

    // Drain real connect/disconnect events first. They always take precedence
    // over a scheduled media reprobe.
    while (true) {
      bool disconnect = false;
      uint8_t address = 0;
      msc_host_device_handle_t device = nullptr;
      portENTER_CRITICAL(&eventLock_);
      if (disconnectPending_) {
        disconnect = true;
        device = pendingDisconnectDevice_;
        disconnectPending_ = false;
        pendingDisconnectDevice_ = nullptr;
      } else if (connectPending_) {
        address = pendingAddress_;
        connectPending_ = false;
        mediaRetryPending_ = false;
      } else {
        portEXIT_CRITICAL(&eventLock_);
        break;
      }
      portEXIT_CRITICAL(&eventLock_);
      if (disconnect) handleDisconnected(device);
      else handleConnected(address);
    }

    if (retryDue && mediaRetryPending_) {
      const uint8_t address = mediaRetryAddress_;
      mediaRetryPending_ = false;
      handleConnected(address);
    }
  }
}

void NewoUsbStorage::mscEvent(const msc_host_event_t* event, void* arg) {
  NewoUsbStorage* storage = static_cast<NewoUsbStorage*>(arg);
  if (event == nullptr || storage == nullptr || storage->workerTask_ == nullptr) return;

  portENTER_CRITICAL(&storage->eventLock_);
  if (event->event == msc_host_event_t::MSC_DEVICE_CONNECTED) {
    storage->pendingAddress_ = event->device.address;
    storage->connectPending_ = true;
  } else if (event->event == msc_host_event_t::MSC_DEVICE_DISCONNECTED) {
    storage->pendingDisconnectDevice_ = event->device.handle;
    storage->disconnectPending_ = true;
  } else {
    portEXIT_CRITICAL(&storage->eventLock_);
    return;
  }
  portEXIT_CRITICAL(&storage->eventLock_);
  xTaskNotifyGive(storage->workerTask_);
}

void NewoUsbStorage::handleConnected(uint8_t address) {
  if (device_ != nullptr) {
    Serial.printf("[usb-storage] MSC_IGNORED — address=%u reason=storage_slot_busy\n",
                  static_cast<unsigned>(address));
    return;
  }

  msc_host_device_handle_t device = nullptr;
  esp_err_t error = msc_host_install_device(address, &device);
  if (error == ESP_ERR_MSC_MOUNT_FAILED) {
    // In Newo's vendored MSC driver this install-time value specifically means
    // SCSI NOT READY / ASC 0x3A (medium not present). Do not hammer TEST UNIT
    // READY at 100 ms or tear down the shared USB host: keep only this address
    // in a low-rate reprobe state so inserted/recovered media is discovered.
    if (!mediaWaiting_) {
      Serial.printf("[usb-storage] MEDIA_ABSENT — address=%u; reprobe=%lums\n",
                    static_cast<unsigned>(address),
                    static_cast<unsigned long>(kMediaRetryMs));
    }
    mediaWaiting_ = true;
    mediaRetryAddress_ = address;
    mediaRetryPending_ = true;
    return;
  }
  if (error != ESP_OK) {
    if (mediaWaiting_) {
      Serial.printf("[usb-storage] MEDIA_PROBE_STOPPED — address=%u reason=%s\n",
                    static_cast<unsigned>(address), esp_err_to_name(error));
    } else {
      logError("MOUNT_FAILED", error);
    }
    mediaWaiting_ = false;
    mediaRetryPending_ = false;
    return;
  }

  if (mediaWaiting_) {
    Serial.printf("[usb-storage] MEDIA_READY — address=%u\n", static_cast<unsigned>(address));
  }
  mediaWaiting_ = false;
  mediaRetryPending_ = false;
  device_ = device;
  Serial.printf("[usb-storage] MSC_CONNECTED — address=%u\n", static_cast<unsigned>(address));

  msc_host_device_info_t info = {};
  error = msc_host_get_device_info(device_, &info);
  if (error != ESP_OK) {
    logError("MOUNT_FAILED", error);
    msc_host_uninstall_device(device_);
    device_ = nullptr;
    return;
  }
  const uint64_t capacity = static_cast<uint64_t>(info.sector_count) * info.sector_size;
  Serial.printf("[usb-storage] capacity=%" PRIu64 " sector=%" PRIu32 "\n", capacity, info.sector_size);

  esp_vfs_fat_mount_config_t mountConfig = {};
  mountConfig.format_if_mount_failed = false;
  mountConfig.max_files = 4;
  mountConfig.allocation_unit_size = 8192;
  error = msc_host_vfs_register(device_, "/usb", &mountConfig, &vfs_);
  if (error != ESP_OK) {
    logError("MOUNT_FAILED", error);
    msc_host_uninstall_device(device_);
    device_ = nullptr;
    vfs_ = nullptr;
    return;
  }

  mounted_ = true;
  Serial.println("[usb-storage] MOUNTED — path=/usb");
}

void NewoUsbStorage::handleDisconnected(msc_host_device_handle_t device) {
  if (device == nullptr || device != device_) return;
  Serial.println("[usb-storage] MSC_DISCONNECTED");
  releaseMountedDevice();
}

void NewoUsbStorage::releaseMountedDevice() {
  mediaRetryPending_ = false;
  mediaWaiting_ = false;
  if (vfs_ != nullptr) {
    const esp_err_t error = msc_host_vfs_unregister(vfs_);
    if (error != ESP_OK) logError("UNMOUNT_FAILED", error);
    vfs_ = nullptr;
    mounted_ = false;
    Serial.println("[usb-storage] UNMOUNTED");
  }
  if (device_ != nullptr) {
    const esp_err_t error = msc_host_uninstall_device(device_);
    if (error != ESP_OK) logError("MSC_RELEASE_FAILED", error);
    device_ = nullptr;
  }
}
