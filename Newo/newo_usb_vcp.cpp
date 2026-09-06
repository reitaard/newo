#include "newo_usb_vcp.h"

#include <esp_err.h>
#include <usb/vcp_ch34x.h>
#include <usb/vcp_cp210x.h>
#include <usb/vcp_ftdi.h>

NewoUsbVcp newoUsbVcp;
NewoUsbVcp* NewoUsbVcp::instance_ = nullptr;

namespace {
constexpr UBaseType_t kWorkerPriority = 1;
constexpr uint32_t kWorkerStack = 5120;
constexpr uint32_t kOpenTimeoutMs = 100;
constexpr uint32_t kTransferTimeoutMs = 40;
void logError(const char* event, esp_err_t error) {
  Serial.printf("[usb-vcp] %s reason=%s (%d)\n", event, esp_err_to_name(error), static_cast<int>(error));
}
}  // namespace

const char* NewoUsbVcp::driverName(Driver driver) {
  switch (driver) {
    case Driver::CDC_ACM: return "cdc-acm";
    case Driver::CH34X: return "ch34x";
    case Driver::CP210X: return "cp210x";
    case Driver::FTDI: return "ftdi";
    default: return "none";
  }
}

bool NewoUsbVcp::begin(NewoUsbHost& host) {
  if (host_ != nullptr) return host_ == &host;
  if (!host.ready() || instance_ != nullptr) {
    Serial.println("[usb-vcp] CLIENT_FAILED reason=host_or_instance");
    return false;
  }
  candidates_ = xQueueCreate(4, sizeof(Candidate));
  txQueue_ = xQueueCreate(kTxQueueDepth, sizeof(TxChunk));
  configQueue_ = xQueueCreate(1, sizeof(SerialConfig));
  rx_ = xStreamBufferCreate(kRxCapacity, 1);
  if (candidates_ == nullptr || txQueue_ == nullptr || configQueue_ == nullptr || rx_ == nullptr) {
    Serial.println("[usb-vcp] CLIENT_FAILED reason=bounded_buffers");
    return false;
  }
  host_ = &host;
  instance_ = this;
  const cdc_acm_host_driver_config_t driverConfig = {
      .driver_task_stack_size = 4096, .driver_task_priority = 3,
      .xCoreID = 0, .new_dev_cb = newDevice,
  };
  if (!host.installCdcClient(driverConfig)) {
    instance_ = nullptr;
    host_ = nullptr;
    return false;
  }
  if (xTaskCreate(workerTaskEntry, "newo-usb-vcp", kWorkerStack, this,
                  kWorkerPriority, &workerTask_) != pdPASS) {
    Serial.println("[usb-vcp] CLIENT_FAILED reason=worker_task");
    host.uninstallCdcClient();
    instance_ = nullptr;
    host_ = nullptr;
    return false;
  }
  Serial.printf("[usb-vcp] CLIENT_READY rx=%u tx_chunk=%u tx_depth=%u timeout_ms=%u\n",
                static_cast<unsigned>(kRxCapacity), static_cast<unsigned>(kTxChunkBytes),
                static_cast<unsigned>(kTxQueueDepth), static_cast<unsigned>(kTransferTimeoutMs));
  return true;
}

void NewoUsbVcp::workerTaskEntry(void* arg) { static_cast<NewoUsbVcp*>(arg)->workerTask(); }

void NewoUsbVcp::newDevice(usb_device_handle_t device) {
  NewoUsbVcp* self = instance_;
  if (self == nullptr || device == nullptr || self->candidates_ == nullptr) return;
  const usb_device_desc_t* desc = nullptr;
  const usb_config_desc_t* config = nullptr;
  usb_device_info_t info = {};
  if (usb_host_get_device_descriptor(device, &desc) != ESP_OK || desc == nullptr ||
      usb_host_get_active_config_descriptor(device, &config) != ESP_OK || config == nullptr ||
      usb_host_device_info(device, &info) != ESP_OK) return;
  const bool cdc = desc->bDeviceClass == 0x02 || configLooksLikeCdc(config);
  const bool known = desc->idVendor == NANJING_QINHENG_MICROE_VID ||
                     desc->idVendor == SILICON_LABS_VID || desc->idVendor == FTDI_VID;
  if (!cdc && !known) return;
  const Candidate candidate = {info.dev_addr, desc->idVendor, desc->idProduct, cdc};
  if (xQueueSend(self->candidates_, &candidate, 0) != pdTRUE)
    Serial.println("[usb-vcp] DISCOVERY_DROPPED reason=candidate_queue_full");
}

bool NewoUsbVcp::configLooksLikeCdc(const usb_config_desc_t* config) {
  if (config == nullptr) return false;
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(config);
  bool control = false, data = false;
  for (size_t pos = 0; pos + 2 <= config->wTotalLength;) {
    const uint8_t length = raw[pos];
    if (length < 2 || pos + length > config->wTotalLength) return false;
    if (raw[pos + 1] == 0x04 && length >= 9) {
      control |= raw[pos + 5] == 0x02 && raw[pos + 6] == 0x02;
      data |= raw[pos + 5] == 0x0a;
    }
    pos += length;
  }
  return control && data;
}

bool NewoUsbVcp::receiveData(const uint8_t* data, size_t length, void* arg) {
  auto* self = static_cast<NewoUsbVcp*>(arg);
  if (self == nullptr || self->rx_ == nullptr || data == nullptr) return true;
  const size_t accepted = xStreamBufferSend(self->rx_, data, length, 0);
  if (accepted < length) self->rxDropped_.fetch_add(length - accepted);
  return true;
}

void NewoUsbVcp::deviceEvent(const cdc_acm_host_dev_event_data_t* event, void* arg) {
  auto* self = static_cast<NewoUsbVcp*>(arg);
  if (self == nullptr || event == nullptr) return;
  if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
    self->ready_.store(false);
    self->disconnectPending_.store(true);
  } else if (event->type == CDC_ACM_HOST_ERROR) {
    self->transferErrors_.fetch_add(1);
  }
}

void NewoUsbVcp::workerTask() {
  Candidate candidate;
  TxChunk tx;
  SerialConfig requestedConfig;
  while (host_ != nullptr) {
    if (disconnectPending_.exchange(false)) closeDevice("unplug");
    if (xQueueReceive(configQueue_, &requestedConfig, 0) == pdTRUE) {
      config_ = requestedConfig;
      if (ready_.load() && !applyConfig(config_)) closeDevice("line_config");
    }
    if (!ready_.load() && xQueueReceive(candidates_, &candidate, pdMS_TO_TICKS(10)) == pdTRUE)
      openCandidate(candidate);
    if (ready_.load() && xQueueReceive(txQueue_, &tx, 0) == pdTRUE) {
      cdc_acm_dev_hdl_t handle = nullptr;
      portENTER_CRITICAL(&deviceLock_); handle = device_; portEXIT_CRITICAL(&deviceLock_);
      const esp_err_t result = handle == nullptr ? ESP_ERR_INVALID_STATE :
          cdc_acm_host_data_tx_blocking(handle, tx.data, tx.length, kTransferTimeoutMs);
      if (result != ESP_OK) {
        transferErrors_.fetch_add(1);
        logError("TX_FAILED", result);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void NewoUsbVcp::openCandidate(const Candidate& candidate) {
  if (device_ != nullptr) return;
  const cdc_acm_host_device_config_t config = {
      .connection_timeout_ms = kOpenTimeoutMs, .out_buffer_size = kTxChunkBytes,
      .in_buffer_size = 256, .event_cb = deviceEvent, .data_cb = receiveData, .user_arg = this,
  };
  cdc_acm_dev_hdl_t opened = nullptr;
  Driver selected = Driver::NONE;
  esp_err_t result = ESP_ERR_NOT_FOUND;
  const bool knownBridge = candidate.vid == NANJING_QINHENG_MICROE_VID ||
                           candidate.vid == SILICON_LABS_VID || candidate.vid == FTDI_VID;
  if (candidate.cdc && !knownBridge) {
    const cdc_acm_host_open_config_t openConfig = {
        .vid = candidate.vid, .pid = candidate.pid, .interface_idx = 0,
        .dev_addr = candidate.address, .connection_timeout_ms = kOpenTimeoutMs,
        .out_buffer_size = kTxChunkBytes, .in_buffer_size = 256,
        .event_cb = deviceEvent, .data_cb = receiveData, .user_arg = this,
    };
    result = cdc_acm_host_open(&openConfig, &opened);
    if (result == ESP_OK) selected = Driver::CDC_ACM;
  }
  if (opened == nullptr && candidate.vid == NANJING_QINHENG_MICROE_VID) {
    result = ch34x_vcp_open(candidate.pid, 0, &config, &opened);
    if (result == ESP_OK) selected = Driver::CH34X;
  } else if (opened == nullptr && candidate.vid == SILICON_LABS_VID) {
    result = cp210x_vcp_open(candidate.pid, 0, &config, &opened);
    if (result == ESP_OK) selected = Driver::CP210X;
  } else if (opened == nullptr && candidate.vid == FTDI_VID) {
    result = ftdi_vcp_open(candidate.pid, 0, &config, &opened);
    if (result == ESP_OK) selected = Driver::FTDI;
  }
  if (result != ESP_OK || opened == nullptr) {
    Serial.printf("[usb-vcp] OPEN_REJECTED address=%u vid=%04x pid=%04x reason=%s channels=clean-fail\n",
                  candidate.address, candidate.vid, candidate.pid, esp_err_to_name(result));
    return;
  }
  portENTER_CRITICAL(&deviceLock_); device_ = opened; portEXIT_CRITICAL(&deviceLock_);
  address_.store(candidate.address); vid_.store(candidate.vid); pid_.store(candidate.pid);
  driver_.store(selected);
  purgeRx();
  if (!applyConfig(config_)) { closeDevice("line_config"); return; }
  generation_.fetch_add(1);
  ready_.store(true);
  Serial.printf("[usb-vcp] VCP_READY address=%u vid=%04x pid=%04x driver=%s generation=%lu\n",
                candidate.address, candidate.vid, candidate.pid, driverName(selected),
                static_cast<unsigned long>(generation_.load()));
}

void NewoUsbVcp::closeDevice(const char* reason) {
  ready_.store(false);
  cdc_acm_dev_hdl_t handle = nullptr;
  portENTER_CRITICAL(&deviceLock_); handle = device_; device_ = nullptr; portEXIT_CRITICAL(&deviceLock_);
  if (handle != nullptr) {
    const esp_err_t result = cdc_acm_host_close(handle);
    if (result != ESP_OK) logError("CLOSE_FAILED", result);
  }
  xQueueReset(txQueue_);
  purgeRx();
  Serial.printf("[usb-vcp] VCP_DISCONNECTED address=%u reason=%s rx_drop=%lu tx_drop=%lu errors=%lu\n",
                address_.exchange(0), reason, static_cast<unsigned long>(rxDropped_.load()),
                static_cast<unsigned long>(txDropped_.load()), static_cast<unsigned long>(transferErrors_.load()));
  vid_.store(0); pid_.store(0); driver_.store(Driver::NONE);
}

bool NewoUsbVcp::applyConfig(const SerialConfig& config) {
  if (device_ == nullptr || config.baud == 0 || config.dataBits < 5 || config.dataBits > 8) return false;
  const cdc_acm_line_coding_t coding = {
      .dwDTERate = config.baud, .bCharFormat = static_cast<uint8_t>(config.stopBits),
      .bParityType = static_cast<uint8_t>(config.parity), .bDataBits = config.dataBits,
  };
  esp_err_t result = cdc_acm_host_line_coding_set(device_, &coding);
  if (result == ESP_OK) result = cdc_acm_host_set_control_line_state(device_, config.dtr, config.rts);
  if (result != ESP_OK) logError("CONFIG_FAILED", result);
  return result == ESP_OK;
}

bool NewoUsbVcp::configure(const SerialConfig& config) {
  if (config.baud == 0 || config.dataBits < 5 || config.dataBits > 8 || configQueue_ == nullptr)
    return false;
  // One-slot mailbox: newest complete configuration wins without blocking the caller.
  return xQueueOverwrite(configQueue_, &config) == pdTRUE;
}

size_t NewoUsbVcp::write(const uint8_t* data, size_t length, uint32_t timeoutMs) {
  if (!ready_.load() || data == nullptr || length > kMaxWriteBytes) return 0;
  size_t queued = 0;
  while (queued < length) {
    TxChunk chunk = {};
    chunk.length = static_cast<uint16_t>(min(length - queued, kTxChunkBytes));
    memcpy(chunk.data, data + queued, chunk.length);
    const TickType_t wait = queued == 0 ? pdMS_TO_TICKS(timeoutMs) : 0;
    if (xQueueSend(txQueue_, &chunk, wait) != pdTRUE) {
      txDropped_.fetch_add(length - queued);
      break;
    }
    queued += chunk.length;
  }
  return queued;
}

size_t NewoUsbVcp::read(uint8_t* data, size_t capacity, uint32_t timeoutMs) {
  if (data == nullptr || capacity == 0 || rx_ == nullptr) return 0;
  return xStreamBufferReceive(rx_, data, capacity, pdMS_TO_TICKS(timeoutMs));
}

void NewoUsbVcp::purgeRx() { if (rx_ != nullptr) xStreamBufferReset(rx_); }
