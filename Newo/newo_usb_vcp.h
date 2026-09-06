#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <usb/cdc_acm_host.h>

#include "newo_usb_host.h"

// Generic, bounded USB virtual-COM transport. Application protocols belong in
// a layer above this class (NewoArduinoNode), never in this driver.
class NewoUsbVcp {
 public:
  enum class Driver : uint8_t { NONE, CDC_ACM, CH34X, CP210X, FTDI };
  enum class Parity : uint8_t { NONE = 0, ODD = 1, EVEN = 2, MARK = 3, SPACE = 4 };
  enum class StopBits : uint8_t { ONE = 0, ONE_POINT_FIVE = 1, TWO = 2 };
  struct SerialConfig {
    uint32_t baud = 115200;
    uint8_t dataBits = 8;
    Parity parity = Parity::NONE;
    StopBits stopBits = StopBits::ONE;
    bool dtr = true;
    bool rts = true;
  };

  static constexpr size_t kRxCapacity = 2048;
  static constexpr size_t kTxChunkBytes = 256;
  static constexpr size_t kTxQueueDepth = 8;
  static constexpr size_t kMaxWriteBytes = kTxChunkBytes * kTxQueueDepth;

  bool begin(NewoUsbHost& host);
  bool ready() const { return ready_.load(); }
  uint32_t generation() const { return generation_.load(); }
  uint8_t address() const { return address_.load(); }
  uint16_t vid() const { return vid_.load(); }
  uint16_t pid() const { return pid_.load(); }
  Driver driver() const { return driver_.load(); }
  bool configure(const SerialConfig& config);
  size_t write(const uint8_t* data, size_t length, uint32_t timeoutMs = 20);
  size_t read(uint8_t* data, size_t capacity, uint32_t timeoutMs = 0);
  void purgeRx();
  uint32_t rxDropped() const { return rxDropped_.load(); }
  uint32_t txDropped() const { return txDropped_.load(); }
  uint32_t transferErrors() const { return transferErrors_.load(); }
  static const char* driverName(Driver driver);

 private:
  struct Candidate { uint8_t address; uint16_t vid; uint16_t pid; bool cdc; };
  struct TxChunk { uint16_t length; uint8_t data[kTxChunkBytes]; };
  static void workerTaskEntry(void* arg);
  static void newDevice(usb_device_handle_t device);
  static bool receiveData(const uint8_t* data, size_t length, void* arg);
  static void deviceEvent(const cdc_acm_host_dev_event_data_t* event, void* arg);
  void workerTask();
  void openCandidate(const Candidate& candidate);
  void closeDevice(const char* reason);
  bool applyConfig(const SerialConfig& config);
  static bool configLooksLikeCdc(const usb_config_desc_t* config);

  static NewoUsbVcp* instance_;
  NewoUsbHost* host_ = nullptr;
  TaskHandle_t workerTask_ = nullptr;
  QueueHandle_t candidates_ = nullptr;
  QueueHandle_t txQueue_ = nullptr;
  QueueHandle_t configQueue_ = nullptr;
  StreamBufferHandle_t rx_ = nullptr;
  cdc_acm_dev_hdl_t device_ = nullptr;
  portMUX_TYPE deviceLock_ = portMUX_INITIALIZER_UNLOCKED;
  SerialConfig config_;
  std::atomic<bool> ready_{false};
  std::atomic<bool> disconnectPending_{false};
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint8_t> address_{0};
  std::atomic<uint16_t> vid_{0};
  std::atomic<uint16_t> pid_{0};
  std::atomic<Driver> driver_{Driver::NONE};
  std::atomic<uint32_t> rxDropped_{0};
  std::atomic<uint32_t> txDropped_{0};
  std::atomic<uint32_t> transferErrors_{0};
};

extern NewoUsbVcp newoUsbVcp;
