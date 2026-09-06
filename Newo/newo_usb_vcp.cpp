#include "newo_usb_vcp.h"

#include <esp_err.h>

NewoUsbVcp newoUsbVcp;

namespace {
constexpr UBaseType_t kVcpClientTaskPriority = 1;
constexpr uint32_t kVcpClientTaskStack = 4096;

void logVcpError(const char* event, esp_err_t error) {
  Serial.printf("[usb-vcp] %s — reason=%s\n", event, esp_err_to_name(error));
}
}  // namespace

bool NewoUsbVcp::begin(NewoUsbHost& host) {
  if (host_ != nullptr) return host_ == &host && client_ != nullptr;
  if (!host.ready()) {
    Serial.println("[usb-vcp] CLIENT_FAILED — reason=host_not_ready");
    return false;
  }

  host_ = &host;
  const usb_host_client_config_t config = {
      .is_synchronous = false,
      .max_num_event_msg = 8,
      .async = {
          .client_event_callback = clientEvent,
          .callback_arg = this,
      },
  };
  if (!host.registerClient(config, &client_, "arduino-vcp")) {
    host_ = nullptr;
    return false;
  }
  if (xTaskCreate(clientTaskEntry, "newo-usb-vcp", kVcpClientTaskStack, this,
                  kVcpClientTaskPriority, &clientTask_) != pdPASS) {
    Serial.println("[usb-vcp] CLIENT_FAILED — reason=client_task");
    host.deregisterClient(client_, "arduino-vcp");
    client_ = nullptr;
    host_ = nullptr;
    return false;
  }

  Serial.println("[usb-vcp] CLIENT_READY — Arduino/VCP discovery idle");
  return true;
}

void NewoUsbVcp::clientTaskEntry(void* arg) {
  static_cast<NewoUsbVcp*>(arg)->clientTask();
}

void NewoUsbVcp::clientEvent(const usb_host_client_event_msg_t* event, void* arg) {
  auto* vcp = static_cast<NewoUsbVcp*>(arg);
  if (event == nullptr || vcp == nullptr) return;
  if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    vcp->handleConnected(event->new_dev.address);
  } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    vcp->handleDisconnected(event->dev_gone.dev_hdl);
  }
}

void NewoUsbVcp::clientTask() {
  while (client_ != nullptr) {
    const esp_err_t result = usb_host_client_handle_events(client_, pdMS_TO_TICKS(10));
    if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
      logVcpError("CLIENT_EVENT_FAILED", result);
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    cleanupGoneDevice();
  }
  clientTask_ = nullptr;
  vTaskDelete(nullptr);
}

bool NewoUsbVcp::configLooksLikeCdc(const usb_config_desc_t* config) {
  if (config == nullptr) return false;
  const uint8_t* raw = reinterpret_cast<const uint8_t*>(config);
  const size_t total = config->wTotalLength;
  bool control = false;
  bool data = false;

  for (size_t pos = 0; pos + 2 <= total;) {
    const uint8_t* descriptor = raw + pos;
    const uint8_t length = descriptor[0];
    if (length < 2 || pos + length > total) break;
    if (descriptor[1] == 0x04 && length >= 9) {
      const uint8_t ifaceClass = descriptor[5];
      const uint8_t ifaceSubclass = descriptor[6];
      if (ifaceClass == 0x02 && ifaceSubclass == 0x02) control = true;  // CDC ACM control
      if (ifaceClass == 0x0a) data = true;                             // CDC data
    }
    pos += length;
  }
  return control && data;
}

bool NewoUsbVcp::knownUsbSerial(uint16_t vid, uint16_t pid) {
  if (vid == 0x2341 || vid == 0x2a03) return true;  // Arduino official/legacy
  if (vid == 0x1a86 && (pid == 0x7523 || pid == 0x5523 || pid == 0x55d4)) return true;  // CH34x
  if (vid == 0x10c4 && pid == 0xea60) return true;  // CP210x
  if (vid == 0x0403 && pid == 0x6001) return true;  // FT232
  return false;
}

void NewoUsbVcp::handleConnected(uint8_t address) {
  if (client_ == nullptr || device_ != nullptr) return;

  usb_device_handle_t device = nullptr;
  const esp_err_t open = usb_host_device_open(client_, address, &device);
  if (open != ESP_OK || device == nullptr) return;

  const usb_device_desc_t* descriptor = nullptr;
  const usb_config_desc_t* config = nullptr;
  const esp_err_t descError = usb_host_get_device_descriptor(device, &descriptor);
  const esp_err_t configError = usb_host_get_active_config_descriptor(device, &config);
  if (descError != ESP_OK || descriptor == nullptr || configError != ESP_OK || config == nullptr) {
    usb_host_device_close(client_, device);
    return;
  }

  const bool vcp = descriptor->bDeviceClass == 0x02 ||
                   configLooksLikeCdc(config) ||
                   knownUsbSerial(descriptor->idVendor, descriptor->idProduct);
  if (!vcp) {
    usb_host_device_close(client_, device);
    return;
  }

  device_ = device;
  address_ = address;
  vid_ = descriptor->idVendor;
  pid_ = descriptor->idProduct;
  removed_.store(false);
  ready_.store(true);
  Serial.printf("[usb-vcp] VCP_READY — address=%u vid=%04x pid=%04x transport=idle\n",
                static_cast<unsigned>(address_), static_cast<unsigned>(vid_),
                static_cast<unsigned>(pid_));
}

void NewoUsbVcp::handleDisconnected(usb_device_handle_t device) {
  if (device == nullptr || device != device_) return;
  ready_.store(false);
  removed_.store(true);
  Serial.printf("[usb-vcp] VCP_DISCONNECTED — address=%u cleanup=pending\n",
                static_cast<unsigned>(address_));
}

void NewoUsbVcp::cleanupGoneDevice() {
  if (!removed_.load() || device_ == nullptr || client_ == nullptr) return;
  const esp_err_t error = usb_host_device_close(client_, device_);
  if (error != ESP_OK) {
    logVcpError("VCP_CLOSE_PENDING", error);
    return;
  }
  device_ = nullptr;
  address_ = 0;
  vid_ = 0;
  pid_ = 0;
  removed_.store(false);
  ready_.store(false);
  Serial.println("[usb-vcp] VCP_CLEANUP_COMPLETE");
}
