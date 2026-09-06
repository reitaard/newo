#include "newo_arduino_node.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NewoArduinoNode newoArduinoNode;

namespace {
constexpr UBaseType_t kTaskPriority = 1;
constexpr uint32_t kTaskStack = 4096;
}

bool NewoArduinoNode::begin(NewoUsbVcp& transport) {
  if (transport_ != nullptr) return transport_ == &transport;
  events_ = xQueueCreate(kEventDepth, sizeof(Event));
  acknowledgements_ = xQueueCreate(kPendingRequests, sizeof(Acknowledgement));
  if (events_ == nullptr || acknowledgements_ == nullptr) {
    Serial.println("[arduino] START_FAILED reason=bounded_queues");
    return false;
  }
  transport_ = &transport;
  if (xTaskCreate(taskEntry, "newo-arduino", kTaskStack, this, kTaskPriority, &task_) != pdPASS) {
    Serial.println("[arduino] START_FAILED reason=task");
    transport_ = nullptr;
    return false;
  }
  Serial.printf("[arduino] PROTOCOL_READY version=1 frame_max=%u pending=%u events=%u\n",
                static_cast<unsigned>(kMaxFrameBytes), static_cast<unsigned>(kPendingRequests),
                static_cast<unsigned>(kEventDepth));
  return true;
}

void NewoArduinoNode::taskEntry(void* arg) { static_cast<NewoArduinoNode*>(arg)->task(); }

void NewoArduinoNode::task() {
  uint8_t bytes[128];
  while (transport_ != nullptr) {
    if (!transport_->ready()) {
      if (observedGeneration_ != 0) onDisconnected();
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (transport_->generation() != observedGeneration_) onConnected(transport_->generation());
    const size_t received = transport_->read(bytes, sizeof(bytes), 5);
    if (received != 0) consume(bytes, received);
    const uint32_t now = millis();
    if (!ready_.load() && handshakeDeadline_ != 0 && static_cast<int32_t>(now - handshakeDeadline_) >= 0) {
      Serial.printf("[arduino] HANDSHAKE_TIMEOUT id=%lu\n", static_cast<unsigned long>(handshakeId_));
      sendHello();
    }
    expireRequests(now);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void NewoArduinoNode::onConnected(uint32_t generation) {
  observedGeneration_ = generation;
  ready_.store(false);
  protocolVersion_.store(0);
  capabilities_[0] = '\0';
  assembler_.reset();
  portENTER_CRITICAL(&pendingLock_);
  for (auto& item : pending_) item.active = false;
  portEXIT_CRITICAL(&pendingLock_);
  xQueueReset(events_);
  xQueueReset(acknowledgements_);
  handshakeId_ = nextRequestId_.fetch_add(1);
  sendHello();
}

bool NewoArduinoNode::sendHello() {
  char hello[96];
  snprintf(hello, sizeof(hello), "NEOWIRE/1 HELLO id=%lu min=1 max=1\n",
           static_cast<unsigned long>(handshakeId_));
  if (sendFrame(hello)) {
    handshakeDeadline_ = millis() + kHandshakeTimeoutMs;
    Serial.printf("[arduino] HANDSHAKE_SENT id=%lu transport_generation=%lu\n",
                  static_cast<unsigned long>(handshakeId_), static_cast<unsigned long>(observedGeneration_));
    return true;
  }
  handshakeDeadline_ = millis() + kHandshakeTimeoutMs;
  return false;
}

void NewoArduinoNode::onDisconnected() {
  ready_.store(false);
  protocolVersion_.store(0);
  observedGeneration_ = 0;
  handshakeDeadline_ = 0;
  assembler_.reset();
  portENTER_CRITICAL(&pendingLock_);
  for (auto& item : pending_) item.active = false;
  portEXIT_CRITICAL(&pendingLock_);
  xQueueReset(events_);
  Serial.println("[arduino] NODE_DISCONNECTED pending=cancelled partial=discarded");
}

void NewoArduinoNode::consume(const uint8_t* data, size_t length) {
  char frame[kMaxFrameBytes];
  for (size_t i = 0; i < length; ++i) {
    const auto result = assembler_.push(data[i], frame, sizeof(frame));
    if (result == NewoArduinoWire::Assembler::Result::FRAME) handleFrame(frame);
    else if (result == NewoArduinoWire::Assembler::Result::MALFORMED) malformedFrames_.fetch_add(1);
  }
}

void NewoArduinoNode::handleFrame(char* frame) {
  if (strncmp(frame, "NEOWIRE/1 ", 10) != 0) { malformedFrames_.fetch_add(1); return; }
  char idText[16] = {}, value[16] = {};
  if (strncmp(frame + 10, "READY", 5) == 0 && (frame[15] == '\0' || frame[15] == ' ')) {
    char resetCause[16] = {}, caps[kMaxCapabilitiesBytes] = {};
    field(frame, "reset", resetCause, sizeof(resetCause));
    Serial.printf("[arduino] RESET_CAUSE %s\n", resetCause[0] ? resetCause : "unknown");
    if (!field(frame, "version", value, sizeof(value)) || strtoul(value, nullptr, 10) != 1) {
      malformedFrames_.fetch_add(1);
      return;
    }
    field(frame, "capabilities", caps, sizeof(caps));
    strlcpy(capabilities_, caps, sizeof(capabilities_));
    protocolVersion_.store(1);
    ready_.store(true);
    handshakeDeadline_ = 0;
    handshakeGeneration_.fetch_add(1);
    Serial.printf("[arduino] HANDSHAKE_READY version=1 capabilities=%s source=peer_ready\n",
                  capabilities_[0] ? capabilities_ : "none");
    return;
  }
  if (strncmp(frame + 10, "HELLO_ACK ", 10) == 0) {
    char caps[kMaxCapabilitiesBytes] = {};
    if (!field(frame, "id", idText, sizeof(idText)) || !field(frame, "version", value, sizeof(value)) ||
        strtoul(idText, nullptr, 10) != handshakeId_ || strtoul(value, nullptr, 10) != 1) {
      malformedFrames_.fetch_add(1); return;
    }
    field(frame, "capabilities", caps, sizeof(caps));
    strlcpy(capabilities_, caps, sizeof(capabilities_));
    if (ready_.load()) return;  // Retransmitted HELLO produced a duplicate ACK.
    protocolVersion_.store(1); ready_.store(true); handshakeDeadline_ = 0;
    handshakeGeneration_.fetch_add(1);
    Serial.printf("[arduino] HANDSHAKE_READY version=1 capabilities=%s\n",
                  capabilities_[0] ? capabilities_ : "none");
    return;
  }
  if (strncmp(frame + 10, "ACK ", 4) == 0) {
    char status[12] = {}, payload[kMaxPayloadBytes * 3] = {};
    if (!field(frame, "id", idText, sizeof(idText)) || !field(frame, "status", status, sizeof(status))) {
      malformedFrames_.fetch_add(1); return;
    }
    const uint32_t id = strtoul(idText, nullptr, 10);
    bool matched = false;
    portENTER_CRITICAL(&pendingLock_);
    for (auto& item : pending_) if (item.active && item.id == id) {
      item.active = false;
      matched = true;
      break;
    }
    portEXIT_CRITICAL(&pendingLock_);
    if (matched) {
      field(frame, "payload", payload, sizeof(payload));
      Acknowledgement ack = {id, strcmp(status, "ok") == 0, {}};
      if (!NewoArduinoWire::decode(payload, ack.payload, sizeof(ack.payload))) { malformedFrames_.fetch_add(1); return; }
      if (xQueueSend(acknowledgements_, &ack, 0) != pdTRUE)
        Serial.println("[arduino] ACK_DROPPED reason=queue_full");
      return;
    }
    Serial.printf("[arduino] ACK_IGNORED id=%lu reason=unknown\n", static_cast<unsigned long>(id));
    return;
  }
  if (strncmp(frame + 10, "EVENT ", 6) == 0) {
    Event event = {};
    if (!field(frame, "name", event.name, sizeof(event.name)) || !validToken(event.name)) {
      malformedFrames_.fetch_add(1); return;
    }
    char encoded[kMaxPayloadBytes * 3] = {};
    field(frame, "payload", encoded, sizeof(encoded));
    if (!NewoArduinoWire::decode(encoded, event.payload, sizeof(event.payload))) { malformedFrames_.fetch_add(1); return; }
    if (xQueueSend(events_, &event, 0) != pdTRUE)
      Serial.printf("[arduino] EVENT_DROPPED name=%s reason=queue_full\n", event.name);
    return;
  }
  malformedFrames_.fetch_add(1);
}

uint32_t NewoArduinoNode::request(const char* command, const char* payload) {
  if (!ready_.load() || !validToken(command)) return 0;
  const uint32_t id = nextRequestId_.fetch_add(1);
  Pending* slot = nullptr;
  portENTER_CRITICAL(&pendingLock_);
  for (auto& item : pending_) if (!item.active) { slot = &item; break; }
  if (slot != nullptr) *slot = {id, millis() + kRequestTimeoutMs, true};
  portEXIT_CRITICAL(&pendingLock_);
  if (slot == nullptr) { Serial.println("[arduino] REQUEST_REJECTED reason=pending_full"); return 0; }
  char frame[kMaxFrameBytes], encoded[kMaxPayloadBytes * 3] = {};
  if (!NewoArduinoWire::encode(payload, encoded, sizeof(encoded))) {
    portENTER_CRITICAL(&pendingLock_); slot->active = false; portEXIT_CRITICAL(&pendingLock_);
    return 0;
  }
  const int length = snprintf(frame, sizeof(frame), "NEOWIRE/1 REQ id=%lu command=%s payload=%s\n",
                              static_cast<unsigned long>(id), command, encoded);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(frame) || !sendFrame(frame)) {
    portENTER_CRITICAL(&pendingLock_); slot->active = false; portEXIT_CRITICAL(&pendingLock_);
    return 0;
  }
  return id;
}

void NewoArduinoNode::expireRequests(uint32_t now) {
  uint32_t expired[kPendingRequests] = {};
  size_t count = 0;
  portENTER_CRITICAL(&pendingLock_);
  for (auto& item : pending_) if (item.active && static_cast<int32_t>(now - item.deadline) >= 0) {
    item.active = false;
    expired[count++] = item.id;
  }
  portEXIT_CRITICAL(&pendingLock_);
  for (size_t i = 0; i < count; ++i) {
    timedOutRequests_.fetch_add(1);
    Serial.printf("[arduino] REQUEST_TIMEOUT id=%lu\n", static_cast<unsigned long>(expired[i]));
  }
}

bool NewoArduinoNode::sendFrame(const char* frame) {
  const size_t length = strlen(frame);
  return length < kMaxFrameBytes && transport_->write(reinterpret_cast<const uint8_t*>(frame), length, 10) == length;
}

const char* NewoArduinoNode::field(const char* frame, const char* key, char* out, size_t capacity) {
  return NewoArduinoWire::field(frame, key, out, capacity);
}

bool NewoArduinoNode::validToken(const char* value) {
  return NewoArduinoWire::token(value);
}

bool NewoArduinoNode::receiveEvent(Event& event, uint32_t timeoutMs) {
  return xQueueReceive(events_, &event, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

bool NewoArduinoNode::receiveAcknowledgement(Acknowledgement& ack, uint32_t timeoutMs) {
  return xQueueReceive(acknowledgements_, &ack, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}
