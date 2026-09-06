#include "newo_usb_host.h"

#include <esp_err.h>

#include "newo_usb_audio_descriptors.h"

NewoUsbHost newoUsbHost;

namespace {
constexpr UBaseType_t kHostTaskPriority = 2;
constexpr uint32_t kHostTaskStack = 4096;

void logHostError(const char* event, esp_err_t error) {
  Serial.printf("[usb-host] %s — reason=%s\n", event, esp_err_to_name(error));
}
}  // namespace

bool NewoUsbHost::begin() {
  if (ready_.load()) return true;
  if (hostInstalled_.load()) return false;

  usb_host_config_t hostConfig = {};
  hostConfig.intr_flags = ESP_INTR_FLAG_LEVEL1;
  hostConfig.enum_filter_cb = nullptr;
  // One global FIFO budget for every device behind the hub. These are the
  // bench-proven ESP32-S3/ESP-IDF 5.5.4 values: MSC bulk OUT remains usable
  // while D07 384-byte periodic OUT packets fit exactly.
  hostConfig.fifo_settings_custom.rx_fifo_lines = NewoUac::kRxLines;
  hostConfig.fifo_settings_custom.nptx_fifo_lines = NewoUac::kNptxLines;
  hostConfig.fifo_settings_custom.ptx_fifo_lines = NewoUac::kPtxLines;

  esp_err_t error = usb_host_install(&hostConfig);
  if (error != ESP_OK) {
    logHostError("HOST_FAILED", error);
    return false;
  }
  hostInstalled_.store(true);

  if (xTaskCreate(hostTaskEntry, "newo-usb-host", kHostTaskStack, this,
                  kHostTaskPriority, &hostTask_) != pdPASS) {
    Serial.println("[usb-host] HOST_FAILED — reason=host_task");
    usb_host_uninstall();
    hostInstalled_.store(false);
    hostTask_ = nullptr;
    return false;
  }

  ready_.store(true);
  Serial.printf("[usb-host] FIFO RX=%u NPTX=%u PTX=%u TOTAL=%u MPS-IN=%u bulk-OUT=%u periodic-OUT=%u\n",
                NewoUac::kRxLines, NewoUac::kNptxLines, NewoUac::kPtxLines,
                NewoUac::kFifoLinesTotal, NewoUac::kInMps,
                NewoUac::kNptxLines * 4, NewoUac::kOutMps);
  Serial.println("[usb-host] HOST_READY — shared manager");
  return true;
}

void NewoUsbHost::hostTaskEntry(void* arg) {
  static_cast<NewoUsbHost*>(arg)->hostTask();
}

void NewoUsbHost::hostTask() {
  while (hostInstalled_.load()) {
    uint32_t eventFlags = 0;
    const esp_err_t error = usb_host_lib_handle_events(portMAX_DELAY, &eventFlags);
    if (error == ESP_OK || error == ESP_ERR_TIMEOUT) continue;
    logHostError("HOST_EVENT_FAILED", error);
    ready_.store(false);
    break;
  }
  hostTask_ = nullptr;
  vTaskDelete(nullptr);
}

bool NewoUsbHost::registerClient(const usb_host_client_config_t& config,
                                 usb_host_client_handle_t* handle,
                                 const char* name) {
  if (!ready_.load() || handle == nullptr || *handle != nullptr) return false;
  const esp_err_t error = usb_host_client_register(&config, handle);
  if (error != ESP_OK) {
    Serial.printf("[usb-host] CLIENT_FAILED — name=%s reason=%s\n",
                  name != nullptr ? name : "unknown", esp_err_to_name(error));
    return false;
  }
  const uint32_t count = directClients_.fetch_add(1) + 1;
  Serial.printf("[usb-host] CLIENT_READY — name=%s direct_clients=%lu\n",
                name != nullptr ? name : "unknown", static_cast<unsigned long>(count));
  return true;
}

bool NewoUsbHost::deregisterClient(usb_host_client_handle_t handle, const char* name) {
  if (handle == nullptr) return true;
  const esp_err_t error = usb_host_client_deregister(handle);
  if (error != ESP_OK) {
    Serial.printf("[usb-host] CLIENT_RELEASE_FAILED — name=%s reason=%s\n",
                  name != nullptr ? name : "unknown", esp_err_to_name(error));
    return false;
  }
  uint32_t previous = directClients_.load();
  while (previous != 0 && !directClients_.compare_exchange_weak(previous, previous - 1)) {}
  Serial.printf("[usb-host] CLIENT_RELEASED — name=%s direct_clients=%lu\n",
                name != nullptr ? name : "unknown",
                static_cast<unsigned long>(directClients_.load()));
  return true;
}

bool NewoUsbHost::installMscClient(const msc_host_driver_config_t& config) {
  if (!ready_.load() || mscInstalled_.load()) return false;
  const esp_err_t error = msc_host_install(&config);
  if (error != ESP_OK) {
    logHostError("MSC_CLIENT_FAILED", error);
    return false;
  }
  mscInstalled_.store(true);
  Serial.println("[usb-host] CLIENT_READY — name=storage-msc");
  return true;
}

bool NewoUsbHost::uninstallMscClient() {
  if (!mscInstalled_.load()) return true;
  const esp_err_t error = msc_host_uninstall();
  if (error != ESP_OK) {
    logHostError("MSC_CLIENT_RELEASE_FAILED", error);
    return false;
  }
  mscInstalled_.store(false);
  Serial.println("[usb-host] CLIENT_RELEASED — name=storage-msc");
  return true;
}
