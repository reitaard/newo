#include <Arduino.h>
#include <avr/wdt.h>

#include "nano_reset_policy.h"

// Capture MCUSR before the Arduino core initialization clears reset context.
uint8_t resetCause __attribute__((section(".noinit")));
void captureResetCause() __attribute__((naked, section(".init3")));
void captureResetCause() {
  resetCause = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

namespace {
constexpr uint8_t kLedPin = LED_BUILTIN;
constexpr size_t kLineCapacity = 160;
char lineBuffer[kLineCapacity] = {};
size_t lineLength = 0;
bool lineDiscard = false;
bool handshakeReady = false;
NanoResetPolicy::TriggerOnce resetTrigger;
uint32_t nextReadyMs = 0;

enum class LedMode : uint8_t { BOOT, OFF, ON, FAST, SLOW, ERROR };
LedMode ledMode = LedMode::BOOT;
uint32_t ledChangedMs = 0;

const char* resetCauseName() {
  return NanoResetPolicy::causeName(resetCause);
}

void setLedMode(LedMode mode) {
  ledMode = mode;
  ledChangedMs = millis();
  digitalWrite(kLedPin, mode == LedMode::ON || mode == LedMode::BOOT || mode == LedMode::ERROR ? HIGH : LOW);
}

const char* field(const char* frame, const char* key) {
  static char value[24];
  char needle[20];
  snprintf(needle, sizeof(needle), "%s=", key);
  const char* start = strstr(frame, needle);
  if (!start || (start != frame && start[-1] != ' ')) return nullptr;
  start += strlen(needle);
  const char* end = strchr(start, ' ');
  const size_t length = end ? static_cast<size_t>(end - start) : strlen(start);
  if (length == 0 || length >= sizeof(value)) return nullptr;
  memcpy(value, start, length);
  value[length] = '\0';
  return value;
}

void sendReady() {
  Serial.print(F("NEOWIRE/1 READY reset="));
  Serial.println(resetCauseName());
  // Newo retransmits one stable HELLO ID while pending. This slow fallback is
  // only for a genuinely lost READY/HELLO pair, not normal handshake pacing.
  nextReadyMs = millis() + 3000;
}

void handleLedRequest(const char* id, const char* payload) {
  bool accepted = true;
  if (!strcmp(payload, "on")) setLedMode(LedMode::ON);
  else if (!strcmp(payload, "off")) setLedMode(LedMode::OFF);
  else if (!strcmp(payload, "blink_fast")) setLedMode(LedMode::FAST);
  else if (!strcmp(payload, "blink_slow")) setLedMode(LedMode::SLOW);
  else if (!strcmp(payload, "error")) setLedMode(LedMode::ERROR);
  else accepted = false;
  Serial.print(F("NEOWIRE/1 ACK id=")); Serial.print(id);
  Serial.print(F(" status=")); Serial.print(accepted ? F("ok") : F("error"));
  Serial.println(F(" payload="));
}

void handleFrame(char* frame) {
  if (!strncmp(frame, "NEOWIRE/1 HELLO ", 16)) {
    const char* idValue = field(frame, "id");
    if (!idValue) return;
    char id[24];
    strncpy(id, idValue, sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    Serial.print(F("NEOWIRE/1 HELLO_ACK id=")); Serial.print(id);
    Serial.println(F(" version=1 capabilities=reset_trigger,led"));
    handshakeReady = true;
    if (resetTrigger.take()) {
      Serial.println(F("NEOWIRE/1 EVENT name=voice_trigger payload=reset"));
    }
    return;
  }
  if (!strncmp(frame, "NEOWIRE/1 REQ ", 14)) {
    const char* idValue = field(frame, "id");
    if (!idValue) return;
    char id[24]; strncpy(id, idValue, sizeof(id) - 1); id[sizeof(id) - 1] = '\0';
    const char* commandValue = field(frame, "command");
    if (!commandValue) return;
    char command[24]; strncpy(command, commandValue, sizeof(command) - 1); command[sizeof(command) - 1] = '\0';
    const char* payload = field(frame, "payload");
    if (!strcmp(command, "led") && payload) handleLedRequest(id, payload);
    else {
      Serial.print(F("NEOWIRE/1 ACK id=")); Serial.print(id);
      Serial.println(F(" status=error payload=unsupported"));
    }
  }
}

void serviceSerial() {
  while (Serial.available()) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\n') {
      if (!lineDiscard && lineLength) {
        if (lineBuffer[lineLength - 1] == '\r') --lineLength;
        lineBuffer[lineLength] = '\0';
        handleFrame(lineBuffer);
      }
      lineLength = 0;
      lineDiscard = false;
    } else if (!lineDiscard) {
      if (ch < 0x20 || ch > 0x7e || lineLength + 1 >= sizeof(lineBuffer)) lineDiscard = true;
      else lineBuffer[lineLength++] = ch;
    }
  }
}

void serviceLed() {
  const uint32_t elapsed = millis() - ledChangedMs;
  switch (ledMode) {
    case LedMode::BOOT:
      if (elapsed >= 120) setLedMode(LedMode::OFF);
      break;
    case LedMode::FAST: digitalWrite(kLedPin, (elapsed / 100) & 1); break;
    case LedMode::SLOW: digitalWrite(kLedPin, (elapsed / 500) & 1); break;
    case LedMode::ERROR:
      if (elapsed >= 600) setLedMode(LedMode::OFF);
      else digitalWrite(kLedPin, (elapsed / 100) % 2 == 0 ? HIGH : LOW);
      break;
    case LedMode::ON:
    case LedMode::OFF: break;
  }
}
}  // namespace

void setup() {
  pinMode(kLedPin, OUTPUT);
  setLedMode(LedMode::BOOT);
  Serial.begin(115200);
  resetTrigger.arm(resetCause);
  sendReady();
}

void loop() {
  serviceSerial();
  serviceLed();
  if (!handshakeReady && static_cast<int32_t>(millis() - nextReadyMs) >= 0) sendReady();
}
