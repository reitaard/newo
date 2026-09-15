#pragma once

#include <Arduino.h>

class NewoConsolePrint : public Print {
 public:
  using Print::write;

  void begin(unsigned long baud);
  size_t write(uint8_t byte) override;
  size_t write(const uint8_t* buffer, size_t size) override;

  bool setRemoteEnabled(bool enabled);
  bool remoteEnabled() const;
  size_t readRemote(uint8_t* destination, size_t capacity, uint32_t* droppedBytes);
  void noteRemoteDrop(size_t bytes);
  static constexpr size_t remoteCapacity() { return 32 * 1024; }

 private:
  void capture(const uint8_t* buffer, size_t size);
};

extern NewoConsolePrint NewoConsole;
