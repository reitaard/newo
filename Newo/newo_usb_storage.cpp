#include "newo_usb_storage.h"

#include <inttypes.h>

#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <freertos/task.h>
#include <usb/usb_host.h>

namespace {
constexpr uint8_t kUsbHubClass = 0x09;
constexpr UBaseType_t kHostTaskPriority = 2;
constexpr UBaseType_t kMscTaskPriority = 2;
constexpr UBaseType_t kMonitorTaskPriority = 1;
constexpr UBaseType_t kWorkerTaskPriority = 1;
constexpr uint32_t kHostTaskStack = 4096;
constexpr uint32_t kMscTaskStack = 4096;
constexpr uint32_t kMonitorTaskStack = 3072;
constexpr uint32_t kWorkerTaskStack = 6144;

const char* speedName(usb_speed_t speed) {
  switch (speed) {
    case USB_SPEED_LOW: return "low";
    case USB_SPEED_FULL: return "full";
    case USB_SPEED_HIGH: return "high";
    default: return "unknown";
  }
}

void logError(const char* event, esp_err_t error) {
  Serial.printf("[usb] %s — reason=%s\n", event, esp_err_to_name(error));
}
}  // namespace

NewoUsbStorage* NewoUsbStorage::instance_ = nullptr;

bool NewoUsbStorage::begin() {
  if (instance_ != nullptr) return instance_ == this;

  instance_ = this;
  TaskHandle_t workerTask = nullptr;
  if (xTaskCreate(workerTaskEntry, "newo-usb-vfs", kWorkerTaskStack, this,
                  kWorkerTaskPriority, &workerTask) != pdPASS) {
    Serial.println("[usb] MOUNT_FAILED — reason=worker_task");
    instance_ = nullptr;
    return false;
  }
  workerTask_ = workerTask;
  if (xTaskCreate(hostTaskEntry, "newo-usb-host", kHostTaskStack, this,
                  kHostTaskPriority, nullptr) != pdPASS) {
    Serial.println("[usb] MOUNT_FAILED — reason=host_task");
    vTaskDelete(workerTask);
    workerTask_ = nullptr;
    instance_ = nullptr;
    return false;
  }

  return true;
}

void NewoUsbStorage::hostTaskEntry(void* arg) {
  static_cast<NewoUsbStorage*>(arg)->hostTask();
}

void NewoUsbStorage::monitorTaskEntry(void* arg) {
  static_cast<NewoUsbStorage*>(arg)->monitorTask();
}

void NewoUsbStorage::workerTaskEntry(void* arg) {
  static_cast<NewoUsbStorage*>(arg)->workerTask();
}

void NewoUsbStorage::hostTask() {
  usb_host_config_t hostConfig = {};
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;
  // NULL is intentional on 3.3.10: no application filter is required.
  hostConfig.enum_filter_cb = nullptr;
  esp_err_t error = usb_host_install(&hostConfig);
  if (error != ESP_OK) {
    logError("HOST_FAILED", error);
    vTaskDelete(nullptr);
    return;
  }
  hostInstalled_ = true;

  msc_host_driver_config_t mscConfig = {};
  mscConfig.create_backround_task = true;
  mscConfig.task_priority = kMscTaskPriority;
  mscConfig.stack_size = kMscTaskStack;
  mscConfig.core_id = tskNO_AFFINITY;
  mscConfig.callback = mscEvent;
  mscConfig.callback_arg = this;
  error = msc_host_install(&mscConfig);
  if (error != ESP_OK) {
    logError("HOST_FAILED", error);
    usb_host_uninstall();
    hostInstalled_ = false;
    vTaskDelete(nullptr);
    return;
  }
  mscInstalled_ = true;

  if (xTaskCreate(monitorTaskEntry, "newo-usb-monitor", kMonitorTaskStack, this,
                  kMonitorTaskPriority, nullptr) != pdPASS) {
    Serial.println("[usb] HOST_FAILED — reason=monitor_task");
  }
  Serial.println("[usb] HOST_READY");

  while (true) {
    uint32_t eventFlags = 0;
    error = usb_host_lib_handle_events(portMAX_DELAY, &eventFlags);
    if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
      logError("HOST_FAILED", error);
      break;
    }
  }

  vTaskDelete(nullptr);
}

void NewoUsbStorage::monitorTask() {
  const usb_host_client_config_t config = {
      .is_synchronous = false,
      .max_num_event_msg = 4,
      .async = {
          .client_event_callback = monitorClientEvent,
          .callback_arg = this,
      },
  };
  const esp_err_t error = usb_host_client_register(&config, &monitorClient_);
  if (error != ESP_OK) {
    logError("HOST_FAILED", error);
    vTaskDelete(nullptr);
    return;
  }

  while (true) {
    const esp_err_t result = usb_host_client_handle_events(monitorClient_, portMAX_DELAY);
    if (result != ESP_OK) {
      logError("HOST_FAILED", result);
      break;
    }
  }

  usb_host_client_deregister(monitorClient_);
  monitorClient_ = nullptr;
  vTaskDelete(nullptr);
}

void NewoUsbStorage::workerTask() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
      } else {
        portEXIT_CRITICAL(&eventLock_);
        break;
      }
      portEXIT_CRITICAL(&eventLock_);
      if (disconnect) handleDisconnected(device);
      else handleConnected(address);
    }
  }
}

void NewoUsbStorage::monitorClientEvent(const usb_host_client_event_msg_t* event, void* arg) {
  if (event == nullptr || arg == nullptr) return;
  if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    static_cast<NewoUsbStorage*>(arg)->logDevice(event->new_dev.address);
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
    // A disconnect must never be dropped behind a slow mount/probe operation.
    storage->pendingDisconnectDevice_ = event->device.handle;
    storage->disconnectPending_ = true;
  } else {
    portEXIT_CRITICAL(&storage->eventLock_);
    return;
  }
  portEXIT_CRITICAL(&storage->eventLock_);
  xTaskNotifyGive(storage->workerTask_);
}

void NewoUsbStorage::logDevice(uint8_t address) {
  usb_device_handle_t device = nullptr;
  if (usb_host_device_open(monitorClient_, address, &device) != ESP_OK) return;

  usb_device_info_t info = {};
  const usb_device_desc_t* descriptor = nullptr;
  const esp_err_t infoError = usb_host_device_info(device, &info);
  const esp_err_t descriptorError = usb_host_get_device_descriptor(device, &descriptor);
  if (infoError == ESP_OK) {
    Serial.printf("[usb] DEVICE_CONNECTED — address=%u speed=%s\n",
                  static_cast<unsigned>(address), speedName(info.speed));
  }
  if (descriptorError == ESP_OK && descriptor != nullptr && descriptor->bDeviceClass == kUsbHubClass) {
    Serial.println("[usb] HUB_CONNECTED");
  }
  usb_host_device_close(monitorClient_, device);
}

void NewoUsbStorage::handleConnected(uint8_t address) {
  if (device_ != nullptr) {
    Serial.println("[usb] MOUNT_FAILED — reason=busy");
    return;
  }

  msc_host_device_handle_t device = nullptr;
  esp_err_t error = msc_host_install_device(address, &device);
  if (error != ESP_OK) {
    logError("MOUNT_FAILED", error);
    return;
  }
  device_ = device;
  Serial.println("[usb] MSC_CONNECTED");

  msc_host_device_info_t info = {};
  error = msc_host_get_device_info(device_, &info);
  if (error != ESP_OK) {
    logError("MOUNT_FAILED", error);
    msc_host_uninstall_device(device_);
    device_ = nullptr;
    return;
  }
  const uint64_t capacity = static_cast<uint64_t>(info.sector_count) * info.sector_size;
  Serial.printf("[usb] capacity=%" PRIu64 " sector=%" PRIu32 "\n", capacity, info.sector_size);

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
  Serial.println("[usb] MOUNTED — path=/usb");
}

void NewoUsbStorage::handleDisconnected(msc_host_device_handle_t device) {
  if (device == nullptr || device != device_) return;
  Serial.println("[usb] MSC_DISCONNECTED");
  releaseMountedDevice();
}

void NewoUsbStorage::releaseMountedDevice() {
  if (vfs_ != nullptr) {
    const esp_err_t error = msc_host_vfs_unregister(vfs_);
    if (error != ESP_OK) logError("UNMOUNT_FAILED", error);
    vfs_ = nullptr;
    mounted_ = false;
    Serial.println("[usb] UNMOUNTED");
  }
  if (device_ != nullptr) {
    const esp_err_t error = msc_host_uninstall_device(device_);
    if (error != ESP_OK) logError("MSC_RELEASE_FAILED", error);
    device_ = nullptr;
  }
}
