#include "newo_usb_audio.h"

#include <cstring>
#include <new>
#include <esp_err.h>

NewoUsbAudio newoUsbAudio;

namespace {
constexpr uint8_t kUac2CurRequest = 0x01;
constexpr uint8_t kUac2ClockSamFreqControl = 0x01;

void logUsbAudioError(const char* action, esp_err_t error) {
  Serial.printf("[usb-uac2] %s — reason=%s\n", action, esp_err_to_name(error));
}
}  // namespace

uint16_t NewoUsbAudio::le16(const uint8_t* value) {
  return static_cast<uint16_t>(value[0]) |
         (static_cast<uint16_t>(value[1]) << 8);
}

uint32_t NewoUsbAudio::le32(const uint8_t* value) {
  return static_cast<uint32_t>(value[0]) |
         (static_cast<uint32_t>(value[1]) << 8) |
         (static_cast<uint32_t>(value[2]) << 16) |
         (static_cast<uint32_t>(value[3]) << 24);
}

void NewoUsbAudio::putLe32(uint8_t* value, uint32_t data) {
  value[0] = static_cast<uint8_t>(data);
  value[1] = static_cast<uint8_t>(data >> 8);
  value[2] = static_cast<uint8_t>(data >> 16);
  value[3] = static_cast<uint8_t>(data >> 24);
}

NewoUsbAudio::PlaybackCandidate NewoUsbAudio::findPlayback(const usb_config_desc_t* config) {
  PlaybackCandidate result;
  if (config == nullptr) return result;

  const uint8_t* raw = reinterpret_cast<const uint8_t*>(config);
  const size_t total = config->wTotalLength;
  uint8_t iface = 0;
  uint8_t alt = 0;
  uint8_t ifaceClass = 0;
  uint8_t ifaceSubclass = 0;
  uint8_t ifaceProtocol = 0;
  uint8_t terminalLink = 0;
  uint8_t channels = 0;
  uint8_t subslot = 0;
  uint8_t bits = 0;
  uint8_t streamingTerminal = 0;
  uint8_t clockId = 0;

  for (size_t pos = 0; pos + 2 <= total;) {
    const uint8_t* descriptor = raw + pos;
    const uint8_t length = descriptor[0];
    const uint8_t type = descriptor[1];
    if (length < 2 || pos + length > total) break;

    if (type == 0x04 && length >= 9) {
      iface = descriptor[2];
      alt = descriptor[3];
      ifaceClass = descriptor[5];
      ifaceSubclass = descriptor[6];
      ifaceProtocol = descriptor[7];
      terminalLink = 0;
      channels = 0;
      subslot = 0;
      bits = 0;
    } else if (type == 0x24 && length >= 3 && ifaceClass == 0x01 && ifaceProtocol == 0x20) {
      const uint8_t subtype = descriptor[2];

      // UAC2 Input Terminal representing the USB streaming source.
      if (ifaceSubclass == 0x01 && subtype == 0x02 && length >= 17 &&
          le16(descriptor + 4) == 0x0101) {
        streamingTerminal = descriptor[3];
        clockId = descriptor[7];
      }

      // UAC2 AudioStreaming AS_GENERAL.
      if (ifaceSubclass == 0x02 && subtype == 0x01 && length >= 16) {
        terminalLink = descriptor[3];
        const uint8_t formatType = descriptor[5];
        const uint32_t formats = le32(descriptor + 6);
        channels = descriptor[10];
        if (formatType != 0x01 || (formats & 0x00000001U) == 0) channels = 0;
      }

      // UAC2 Type-I format descriptor.
      if (ifaceSubclass == 0x02 && subtype == 0x02 && length >= 6 && descriptor[3] == 0x01) {
        subslot = descriptor[4];
        bits = descriptor[5];
      }
    } else if (type == 0x05 && length >= 7 && ifaceClass == 0x01 &&
               ifaceSubclass == 0x02 && ifaceProtocol == 0x20) {
      const uint8_t endpoint = descriptor[2];
      const uint8_t attributes = descriptor[3];
      const uint16_t mps = le16(descriptor + 4) & 0x07ff;
      const bool outputIsochronous = ((endpoint & 0x80) == 0) && ((attributes & 0x03) == 0x01);
      if (outputIsochronous && channels == 2 && subslot == 2 && bits == 16 &&
          terminalLink != 0 && terminalLink == streamingTerminal && clockId != 0 &&
          mps >= kPacketBytes && mps <= 384) {
        result.iface = iface;
        result.alt = alt;
        result.endpoint = endpoint;
        result.clockId = clockId;
        result.mps = mps;
        result.valid = true;
        return result;
      }
    }

    pos += length;
  }

  return result;
}

bool NewoUsbAudio::begin(usb_host_client_handle_t client) {
  client_ = client;
  Serial.println("[usb-uac2] production D07 playback ready; preferred output when connected");
  return client_ != nullptr;
}

bool NewoUsbAudio::connected(usb_device_handle_t device, uint8_t address) {
  if (client_ == nullptr || device == nullptr) return false;

  const usb_device_desc_t* deviceDescriptor = nullptr;
  if (usb_host_get_device_descriptor(device, &deviceDescriptor) != ESP_OK ||
      deviceDescriptor == nullptr || deviceDescriptor->idVendor != kD07Vid ||
      deviceDescriptor->idProduct != kD07Pid) {
    return false;
  }

  const usb_config_desc_t* config = nullptr;
  const esp_err_t configError = usb_host_get_active_config_descriptor(device, &config);
  if (configError != ESP_OK || config == nullptr) {
    logUsbAudioError("D07 descriptor read failed", configError);
    return false;
  }

  const PlaybackCandidate candidate = findPlayback(config);
  if (!candidate.valid) {
    Serial.println("[usb-uac2] D07 found but proven PCM16 stereo alternate is unavailable");
    return false;
  }

  // The monitor keeps exactly one D07 reference. A second audio device remains
  // diagnostics-only and is closed by NewoUsbStorage.
  if (device_ != nullptr) {
    Serial.println("[usb-uac2] another D07 reference is already retained");
    return false;
  }

  device_ = device;
  address_ = address;
  candidate_ = candidate;
  removed_.store(false);
  ready_.store(true);
  Serial.printf("[usb-uac2] D07_READY — address=%u iface=%u alt=%u ep=0x%02x MPS=%u clock=%u output=48000Hz PCM16 stereo\n",
                static_cast<unsigned>(address_), static_cast<unsigned>(candidate_.iface),
                static_cast<unsigned>(candidate_.alt), static_cast<unsigned>(candidate_.endpoint),
                static_cast<unsigned>(candidate_.mps), static_cast<unsigned>(candidate_.clockId));
  return true;
}

void NewoUsbAudio::disconnected(usb_device_handle_t device) {
  if (device == nullptr || device != device_) return;
  ready_.store(false);
  removed_.store(true);
  Serial.printf("[usb-uac2] D07_DISCONNECTED — address=%u cleanup=pending\n",
                static_cast<unsigned>(address_));
}

void NewoUsbAudio::controlDone(usb_transfer_t* transfer) {
  if (transfer == nullptr || transfer->context == nullptr) return;
  auto* wait = static_cast<ControlWait*>(transfer->context);
  wait->status = transfer->status;
  wait->actualBytes = transfer->actual_num_bytes;

  ControlState expected = ControlState::WAITING;
  if (wait->state.compare_exchange_strong(expected, ControlState::COMPLETED)) {
    // Publish COMPLETED before waking the caller. Once the semaphore is given,
    // this callback must not touch wait/transfer again because the caller owns
    // and may immediately free both.
    xSemaphoreGive(wait->done);
    return;
  }

  if (expected == ControlState::ABANDONED) {
    // The caller timed out and deliberately transferred ownership to us. The
    // callback means the transfer is no longer in-flight, so it is now legal to
    // release the transfer and its heap-owned wait context.
    SemaphoreHandle_t done = wait->done;
    transfer->context = nullptr;
    usb_host_transfer_free(transfer);
    if (done != nullptr) vSemaphoreDelete(done);
    delete wait;
  }
}

esp_err_t NewoUsbAudio::controlRequest(uint8_t requestType, uint8_t request, uint16_t value,
                                       uint16_t index, void* data, uint16_t length) {
  if (client_ == nullptr || device_ == nullptr || removed_.load()) return ESP_ERR_INVALID_STATE;

  auto* wait = new (std::nothrow) ControlWait();
  if (wait == nullptr) return ESP_ERR_NO_MEM;
  wait->done = xSemaphoreCreateBinary();
  if (wait->done == nullptr) {
    delete wait;
    return ESP_ERR_NO_MEM;
  }

  usb_transfer_t* transfer = nullptr;
  esp_err_t error = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + length, 0, &transfer);
  if (error != ESP_OK) {
    vSemaphoreDelete(wait->done);
    delete wait;
    return error;
  }

  auto* setup = reinterpret_cast<usb_setup_packet_t*>(transfer->data_buffer);
  setup->bmRequestType = requestType;
  setup->bRequest = request;
  setup->wValue = value;
  setup->wIndex = index;
  setup->wLength = length;

  const bool input = (requestType & USB_BM_REQUEST_TYPE_DIR_IN) != 0;
  if (!input && length > 0 && data != nullptr) {
    memcpy(transfer->data_buffer + sizeof(usb_setup_packet_t), data, length);
  }

  transfer->device_handle = device_;
  transfer->bEndpointAddress = 0;
  transfer->callback = controlDone;
  transfer->context = wait;
  transfer->timeout_ms = 1000;
  transfer->num_bytes = sizeof(usb_setup_packet_t) + length;

  error = usb_host_transfer_submit_control(client_, transfer);
  if (error != ESP_OK) {
    transfer->context = nullptr;
    usb_host_transfer_free(transfer);
    vSemaphoreDelete(wait->done);
    delete wait;
    return error;
  }

  if (xSemaphoreTake(wait->done, pdMS_TO_TICKS(1500)) != pdTRUE) {
    ControlState expected = ControlState::WAITING;
    if (wait->state.compare_exchange_strong(expected, ControlState::ABANDONED)) {
      // Do not free anything here: the asynchronous callback still owns an
      // in-flight transfer. It will free transfer + semaphore + context after
      // completion, including hot-unplug/NO_DEVICE completion.
      Serial.printf("[usb-uac2] control request 0x%02x timed out; callback owns cleanup\n", request);
      return ESP_ERR_TIMEOUT;
    }

    // The callback won the race and marked COMPLETED just as our timed wait
    // expired. It committed to giving the semaphore immediately, so wait for
    // that hand-off rather than misreporting a timeout or freeing underneath it.
    if (expected == ControlState::COMPLETED) {
      xSemaphoreTake(wait->done, portMAX_DELAY);
    } else {
      return ESP_ERR_INVALID_STATE;
    }
  }

  if (wait->status != USB_TRANSFER_STATUS_COMPLETED) {
    error = ESP_FAIL;
  } else if (input && length > 0 && data != nullptr) {
    memcpy(data, transfer->data_buffer + sizeof(usb_setup_packet_t), length);
  }

  transfer->context = nullptr;
  usb_host_transfer_free(transfer);
  vSemaphoreDelete(wait->done);
  delete wait;
  return error;
}

esp_err_t NewoUsbAudio::setClockRate(uint32_t rate) {
  uint8_t payload[4];
  putLe32(payload, rate);
  return controlRequest(USB_BM_REQUEST_TYPE_DIR_OUT |
                            USB_BM_REQUEST_TYPE_TYPE_CLASS |
                            USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                        kUac2CurRequest,
                        static_cast<uint16_t>(kUac2ClockSamFreqControl) << 8,
                        (static_cast<uint16_t>(candidate_.clockId) << 8) | kControlInterface,
                        payload, sizeof(payload));
}

esp_err_t NewoUsbAudio::getClockRate(uint32_t* rate) {
  uint8_t payload[4] = {};
  const esp_err_t error = controlRequest(USB_BM_REQUEST_TYPE_DIR_IN |
                                             USB_BM_REQUEST_TYPE_TYPE_CLASS |
                                             USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                                         kUac2CurRequest,
                                         static_cast<uint16_t>(kUac2ClockSamFreqControl) << 8,
                                         (static_cast<uint16_t>(candidate_.clockId) << 8) | kControlInterface,
                                         payload, sizeof(payload));
  if (error == ESP_OK && rate != nullptr) *rate = le32(payload);
  return error;
}

esp_err_t NewoUsbAudio::setInterface(uint8_t alt) {
  return controlRequest(USB_BM_REQUEST_TYPE_DIR_OUT |
                            USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                            USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                        USB_B_REQUEST_SET_INTERFACE, alt, candidate_.iface, nullptr, 0);
}

void NewoUsbAudio::resetTransportStats() {
  transferErrors_.store(0);
  packetErrors_.store(0);
  completedPackets_.store(0);
  completedBytes_.store(0);
}

bool NewoUsbAudio::allocateTransfers() {
  if (freeTransfers_ != nullptr || drained_ != nullptr) return false;
  freeTransfers_ = xQueueCreate(kTransferCount, sizeof(usb_transfer_t*));
  drained_ = xSemaphoreCreateBinary();
  if (freeTransfers_ == nullptr || drained_ == nullptr) {
    releaseTransfers();
    return false;
  }

  for (uint8_t i = 0; i < kTransferCount; ++i) {
    esp_err_t error = usb_host_transfer_alloc(kTransferBytes, kPacketsPerTransfer, &transfers_[i]);
    if (error != ESP_OK || transfers_[i] == nullptr) {
      logUsbAudioError("transfer allocation failed", error);
      releaseTransfers();
      return false;
    }
    transfers_[i]->device_handle = device_;
    transfers_[i]->bEndpointAddress = candidate_.endpoint;
    transfers_[i]->callback = speakerTransferDone;
    transfers_[i]->context = this;
    transfers_[i]->timeout_ms = 1000;
    usb_transfer_t* transfer = transfers_[i];
    if (xQueueSend(freeTransfers_, &transfer, 0) != pdTRUE) {
      releaseTransfers();
      return false;
    }
  }
  return true;
}

void NewoUsbAudio::releaseTransfers() {
  if (activeTransfers_.load() != 0) return;
  if (freeTransfers_ != nullptr) {
    vQueueDelete(freeTransfers_);
    freeTransfers_ = nullptr;
  }
  if (drained_ != nullptr) {
    vSemaphoreDelete(drained_);
    drained_ = nullptr;
  }
  for (uint8_t i = 0; i < kTransferCount; ++i) {
    if (transfers_[i] != nullptr) {
      usb_host_transfer_free(transfers_[i]);
      transfers_[i] = nullptr;
    }
  }
}

bool NewoUsbAudio::beginSpeakerPlayback() {
  if (!speakerReady() || playing_.load() || interfaceClaimed_.load()) return false;
  resetTransportStats();
  activeTransfers_.store(0);

  if (!allocateTransfers()) {
    Serial.println("[usb-uac2] SPEAKER_START_FAILED — reason=transfer_resources");
    return false;
  }

  const esp_err_t setRate = setClockRate(kOutputRate);
  if (setRate != ESP_OK) {
    Serial.printf("[usb-uac2] clock SET_CUR warning=%s; verifying GET_CUR\n", esp_err_to_name(setRate));
  }
  uint32_t actualRate = 0;
  const esp_err_t getRate = getClockRate(&actualRate);
  if (getRate != ESP_OK || actualRate != kOutputRate) {
    Serial.printf("[usb-uac2] SPEAKER_START_FAILED — clock=%lu reason=%s\n",
                  static_cast<unsigned long>(actualRate), esp_err_to_name(getRate));
    releaseTransfers();
    return false;
  }

  esp_err_t error = usb_host_interface_claim(client_, device_, candidate_.iface, candidate_.alt);
  if (error != ESP_OK) {
    logUsbAudioError("interface claim failed", error);
    releaseTransfers();
    return false;
  }
  interfaceClaimed_.store(true);

  error = setInterface(candidate_.alt);
  if (error != ESP_OK) {
    logUsbAudioError("SET_INTERFACE failed", error);
    usb_host_interface_release(client_, device_, candidate_.iface);
    interfaceClaimed_.store(false);
    releaseTransfers();
    return false;
  }

  playing_.store(true);
  Serial.printf("[usb-uac2] SPEAKER_ACTIVE — source=24000Hz mono output=48000Hz stereo ep=0x%02x batches=%u batch_ms=8\n",
                static_cast<unsigned>(candidate_.endpoint), static_cast<unsigned>(kTransferCount));
  return true;
}

void NewoUsbAudio::speakerTransferDone(usb_transfer_t* transfer) {
  if (transfer == nullptr || transfer->context == nullptr) return;
  static_cast<NewoUsbAudio*>(transfer->context)->onSpeakerTransferDone(transfer);
}

void NewoUsbAudio::onSpeakerTransferDone(usb_transfer_t* transfer) {
  bool transferOk = transfer->status == USB_TRANSFER_STATUS_COMPLETED;
  if (!transferOk) transferErrors_.fetch_add(1);
  completedBytes_.fetch_add(static_cast<uint32_t>(transfer->actual_num_bytes));

  for (uint8_t packet = 0; packet < kPacketsPerTransfer; ++packet) {
    if (transfer->isoc_packet_desc[packet].status == USB_TRANSFER_STATUS_COMPLETED) {
      completedPackets_.fetch_add(1);
    } else {
      packetErrors_.fetch_add(1);
      transferOk = false;
    }
  }

  if (freeTransfers_ != nullptr) {
    usb_transfer_t* completed = transfer;
    if (xQueueSend(freeTransfers_, &completed, 0) != pdTRUE) transferErrors_.fetch_add(1);
  }

  const uint32_t previous = activeTransfers_.fetch_sub(1);
  if (previous <= 1 && drained_ != nullptr) xSemaphoreGive(drained_);
}

bool NewoUsbAudio::writeSpeakerMono24(const int16_t* samples, size_t sampleCount,
                                      uint32_t timeoutMs) {
  if (samples == nullptr || sampleCount == 0 || sampleCount > kMono24SamplesPerBatch ||
      !playing_.load() || removed_.load() || freeTransfers_ == nullptr) {
    return false;
  }

  usb_transfer_t* transfer = nullptr;
  if (xQueueReceive(freeTransfers_, &transfer, pdMS_TO_TICKS(timeoutMs)) != pdTRUE || transfer == nullptr) {
    transferErrors_.fetch_add(1);
    return false;
  }

  int16_t* output = reinterpret_cast<int16_t*>(transfer->data_buffer);
  for (size_t i = 0; i < kMono24SamplesPerBatch; ++i) {
    const int16_t sample = i < sampleCount ? samples[i] : 0;
    // Two identical 48 kHz frames for every 24 kHz source sample. Each frame
    // is stereo, so one mono input sample expands to L,R,L,R.
    *output++ = sample;
    *output++ = sample;
    *output++ = sample;
    *output++ = sample;
  }
  for (uint8_t packet = 0; packet < kPacketsPerTransfer; ++packet) {
    transfer->isoc_packet_desc[packet].num_bytes = kPacketBytes;
  }
  transfer->num_bytes = kTransferBytes;

  const uint32_t previous = activeTransfers_.fetch_add(1);
  if (previous == 0 && drained_ != nullptr) xSemaphoreTake(drained_, 0);
  const esp_err_t error = usb_host_transfer_submit(transfer);
  if (error != ESP_OK) {
    transferErrors_.fetch_add(1);
    const uint32_t activeBefore = activeTransfers_.fetch_sub(1);
    if (activeBefore <= 1 && drained_ != nullptr) xSemaphoreGive(drained_);
    if (freeTransfers_ != nullptr) xQueueSend(freeTransfers_, &transfer, 0);
    return false;
  }
  return true;
}

bool NewoUsbAudio::waitForDrain(uint32_t timeoutMs) {
  if (activeTransfers_.load() == 0) return true;
  if (drained_ == nullptr) return false;
  if (xSemaphoreTake(drained_, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  return activeTransfers_.load() == 0;
}

bool NewoUsbAudio::endSpeakerPlayback(uint32_t* drainMs) {
  const uint32_t started = millis();
  const bool wasPlaying = playing_.exchange(false);
  if (!wasPlaying && !interfaceClaimed_.load()) {
    if (drainMs != nullptr) *drainMs = 0;
    return true;
  }

  bool drained = waitForDrain(1500);
  if (!drained && !removed_.load() && device_ != nullptr) {
    Serial.println("[usb-uac2] drain timeout; halting speaker endpoint");
    usb_host_endpoint_halt(device_, candidate_.endpoint);
    usb_host_endpoint_flush(device_, candidate_.endpoint);
    usb_host_endpoint_clear(device_, candidate_.endpoint);
    drained = waitForDrain(500);
  }

  bool controlsOk = true;
  if (interfaceClaimed_.load()) {
    if (!removed_.load() && device_ != nullptr) {
      const esp_err_t alt0 = setInterface(0);
      if (alt0 != ESP_OK) {
        controlsOk = false;
        logUsbAudioError("SET_INTERFACE alt0 failed", alt0);
      }
      const esp_err_t release = usb_host_interface_release(client_, device_, candidate_.iface);
      if (release != ESP_OK) {
        controlsOk = false;
        logUsbAudioError("interface release failed", release);
      }
    }
    interfaceClaimed_.store(false);
  }

  if (drained) releaseTransfers();
  if (drainMs != nullptr) *drainMs = millis() - started;

  const bool healthy = drained && controlsOk && transferErrors_.load() == 0 && packetErrors_.load() == 0;
  Serial.printf("[usb-uac2] SPEAKER_STOP — healthy=%u packets=%lu bytes=%lu transfer-errors=%lu packet-errors=%lu drain_ms=%lu\n",
                healthy ? 1U : 0U,
                static_cast<unsigned long>(completedPackets_.load()),
                static_cast<unsigned long>(completedBytes_.load()),
                static_cast<unsigned long>(transferErrors_.load()),
                static_cast<unsigned long>(packetErrors_.load()),
                static_cast<unsigned long>(millis() - started));
  return healthy;
}

void NewoUsbAudio::service() {
  // Device close belongs to the monitor/client owner and must happen only after
  // every in-flight transfer callback has returned.
  if (!removed_.load() || playing_.load() || activeTransfers_.load() != 0) return;
  releaseTransfers();
  interfaceClaimed_.store(false);
  if (device_ != nullptr) {
    const esp_err_t error = usb_host_device_close(client_, device_);
    if (error != ESP_OK) {
      logUsbAudioError("D07 close pending", error);
      return;
    }
  }
  device_ = nullptr;
  address_ = 0;
  candidate_ = {};
  removed_.store(false);
  ready_.store(false);
  Serial.println("[usb-uac2] D07_CLEANUP_COMPLETE");
}
