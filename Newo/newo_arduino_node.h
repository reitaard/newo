#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "newo_usb_vcp.h"
#include "newo_arduino_wire.h"

// Owns the Newo <-> Arduino protocol. Transport details stay in NewoUsbVcp.
// Wire format is bounded ASCII, one frame per LF:
//   NEOWIRE/1 HELLO id=N min=1 max=1
//   NEOWIRE/1 READY reset=external
//   NEOWIRE/1 HELLO_ACK id=N version=1 capabilities=csv
//   NEOWIRE/1 REQ id=N command=name payload=escaped-text
//   NEOWIRE/1 ACK id=N status=ok payload=escaped-text
//   NEOWIRE/1 EVENT name=name payload=escaped-text
class NewoArduinoNode {
 public:
  static constexpr size_t kMaxFrameBytes = 256;
  static constexpr size_t kMaxNameBytes = 32;
  static constexpr size_t kMaxPayloadBytes = 128;
  static constexpr size_t kMaxCapabilitiesBytes = 96;
  static constexpr size_t kPendingRequests = 8;
  static constexpr size_t kEventDepth = 8;
  static constexpr uint32_t kHandshakeTimeoutMs = 1500;
  static constexpr uint32_t kRequestTimeoutMs = 2000;

  struct Event {
    char name[kMaxNameBytes];
    char payload[kMaxPayloadBytes];
  };
  struct Acknowledgement {
    uint32_t requestId;
    bool success;
    char payload[kMaxPayloadBytes];
  };

  bool begin(NewoUsbVcp& transport);
  bool ready() const { return ready_.load(); }
  uint16_t protocolVersion() const { return protocolVersion_.load(); }
  uint32_t handshakeGeneration() const { return handshakeGeneration_.load(); }
  const char* capabilities() const { return capabilities_; }
  uint32_t request(const char* command, const char* payload = nullptr);
  bool receiveEvent(Event& event, uint32_t timeoutMs = 0);
  bool receiveAcknowledgement(Acknowledgement& ack, uint32_t timeoutMs = 0);
  uint32_t malformedFrames() const { return malformedFrames_.load(); }
  uint32_t timedOutRequests() const { return timedOutRequests_.load(); }

 private:
  struct Pending { uint32_t id; uint32_t deadline; bool active; };
  static void taskEntry(void* arg);
  void task();
  void onConnected(uint32_t generation);
  void onDisconnected();
  void consume(const uint8_t* data, size_t length);
  void handleFrame(char* frame);
  void expireRequests(uint32_t now);
  bool sendFrame(const char* frame);
  static const char* field(const char* frame, const char* key, char* out, size_t capacity);
  static bool validToken(const char* value);

  NewoUsbVcp* transport_ = nullptr;
  TaskHandle_t task_ = nullptr;
  QueueHandle_t events_ = nullptr;
  QueueHandle_t acknowledgements_ = nullptr;
  Pending pending_[kPendingRequests] = {};
  portMUX_TYPE pendingLock_ = portMUX_INITIALIZER_UNLOCKED;
  NewoArduinoWire::Assembler assembler_;
  char capabilities_[kMaxCapabilitiesBytes] = {};
  uint32_t observedGeneration_ = 0;
  uint32_t handshakeId_ = 0;
  uint32_t handshakeDeadline_ = 0;
  std::atomic<uint32_t> nextRequestId_{1};
  std::atomic<bool> ready_{false};
  std::atomic<uint16_t> protocolVersion_{0};
  std::atomic<uint32_t> handshakeGeneration_{0};
  std::atomic<uint32_t> malformedFrames_{0};
  std::atomic<uint32_t> timedOutRequests_{0};
};

extern NewoArduinoNode newoArduinoNode;
