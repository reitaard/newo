#include "newo_console.h"

#include <esp_heap_caps.h>

namespace {

uint8_t* remoteBuffer = nullptr;
size_t remoteHead = 0;
size_t remoteCount = 0;
uint32_t remoteDroppedBytes = 0;
portMUX_TYPE remoteMux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

NewoConsolePrint NewoConsole;

void NewoConsolePrint::begin(unsigned long baud) { Serial.begin(baud); }

size_t NewoConsolePrint::write(uint8_t byte) { return write(&byte, 1); }

size_t NewoConsolePrint::write(const uint8_t* buffer, size_t size) {
  if (!buffer || size == 0) return 0;
  capture(buffer, size);
  return Serial.write(buffer, size);
}

void NewoConsolePrint::capture(const uint8_t* buffer, size_t size) {
  portENTER_CRITICAL(&remoteMux);
  if (!remoteBuffer) {
    portEXIT_CRITICAL(&remoteMux);
    return;
  }
  for (size_t i = 0; i < size; ++i) {
    if (remoteCount == remoteCapacity()) {
      remoteHead = (remoteHead + 1) % remoteCapacity();
      --remoteCount;
      if (remoteDroppedBytes != UINT32_MAX) ++remoteDroppedBytes;
    }
    const size_t tail = (remoteHead + remoteCount) % remoteCapacity();
    remoteBuffer[tail] = buffer[i];
    ++remoteCount;
  }
  portEXIT_CRITICAL(&remoteMux);
}

bool NewoConsolePrint::setRemoteEnabled(bool enabled) {
  if (enabled) {
    portENTER_CRITICAL(&remoteMux);
    const bool alreadyEnabled = remoteBuffer != nullptr;
    portEXIT_CRITICAL(&remoteMux);
    if (alreadyEnabled) return true;

    uint8_t* allocated = static_cast<uint8_t*>(
        heap_caps_malloc(remoteCapacity(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!allocated) return false;

    portENTER_CRITICAL(&remoteMux);
    if (!remoteBuffer) {
      remoteBuffer = allocated;
      allocated = nullptr;
      remoteHead = 0;
      remoteCount = 0;
      remoteDroppedBytes = 0;
    }
    portEXIT_CRITICAL(&remoteMux);
    if (allocated) heap_caps_free(allocated);
    return true;
  }

  uint8_t* released = nullptr;
  portENTER_CRITICAL(&remoteMux);
  released = remoteBuffer;
  remoteBuffer = nullptr;
  remoteHead = 0;
  remoteCount = 0;
  remoteDroppedBytes = 0;
  portEXIT_CRITICAL(&remoteMux);
  if (released) heap_caps_free(released);
  return true;
}

bool NewoConsolePrint::remoteEnabled() const {
  portENTER_CRITICAL(&remoteMux);
  const bool enabled = remoteBuffer != nullptr;
  portEXIT_CRITICAL(&remoteMux);
  return enabled;
}

size_t NewoConsolePrint::readRemote(uint8_t* destination, size_t capacity,
                                   uint32_t* droppedBytes) {
  if (!destination || capacity == 0) return 0;
  portENTER_CRITICAL(&remoteMux);
  if (droppedBytes) {
    *droppedBytes = remoteDroppedBytes;
    remoteDroppedBytes = 0;
  }
  const size_t copied = min(capacity, remoteCount);
  for (size_t i = 0; i < copied; ++i) {
    destination[i] = remoteBuffer[(remoteHead + i) % remoteCapacity()];
  }
  remoteHead = (remoteHead + copied) % remoteCapacity();
  remoteCount -= copied;
  portEXIT_CRITICAL(&remoteMux);
  return copied;
}

void NewoConsolePrint::noteRemoteDrop(size_t bytes) {
  portENTER_CRITICAL(&remoteMux);
  const uint32_t bounded = bytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(bytes);
  remoteDroppedBytes = UINT32_MAX - remoteDroppedBytes < bounded
                           ? UINT32_MAX
                           : remoteDroppedBytes + bounded;
  portEXIT_CRITICAL(&remoteMux);
}
