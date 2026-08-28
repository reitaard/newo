#include "newo_log.h"

#include <cstring>

namespace NewoLog {
namespace {

Entry entries[kCapacity] = {};
size_t head = 0;
size_t count = 0;
uint32_t nextSequence = 1;
uint32_t warnings = 0;
uint32_t errors = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void copyText(char* destination, size_t size, const char* source) {
  if (size == 0) return;
  if (!source) {
    destination[0] = '\0';
    return;
  }
  strncpy(destination, source, size - 1);
  destination[size - 1] = '\0';
}

}  // namespace

const char* levelName(Level level) {
  switch (level) {
    case Level::INFO: return "info";
    case Level::WARN: return "warn";
    case Level::ERROR: return "error";
  }
  return "error";
}

const char* subsystemName(Subsystem subsystem) {
  switch (subsystem) {
    case Subsystem::BOOT: return "boot";
    case Subsystem::SYSTEM: return "system";
    case Subsystem::STORAGE: return "storage";
    case Subsystem::WIFI: return "wifi";
    case Subsystem::PROV: return "prov";
    case Subsystem::CLOUD: return "cloud";
  }
  return "system";
}

void log(Level level, Subsystem subsystem, const char* code, const char* detail) {
  const uint32_t now = millis();
  const char* safeCode = code ? code : "UNKNOWN";
  const char* safeDetail = detail ? detail : "";

  portENTER_CRITICAL(&mux);
  Entry* newest = count == 0 ? nullptr : &entries[(head + kCapacity - 1) % kCapacity];
  if (newest && newest->level == level && newest->subsystem == subsystem &&
      strcmp(newest->code, safeCode) == 0 && strcmp(newest->detail, safeDetail) == 0) {
    newest->lastMs = now;
    if (newest->repeat != UINT16_MAX) ++newest->repeat;
  } else {
    Entry& entry = entries[head];
    entry.sequence = nextSequence++;
    entry.firstMs = now;
    entry.lastMs = now;
    entry.repeat = 1;
    entry.level = level;
    entry.subsystem = subsystem;
    copyText(entry.code, sizeof(entry.code), safeCode);
    copyText(entry.detail, sizeof(entry.detail), safeDetail);
    head = (head + 1) % kCapacity;
    if (count < kCapacity) ++count;
  }
  if (level == Level::WARN) ++warnings;
  if (level == Level::ERROR) ++errors;
  portEXIT_CRITICAL(&mux);

  // Printing is deliberately outside the lock so callback-safe metadata
  // updates never wait on serial I/O.
  Serial.printf("[%s] %s%s%s\n", subsystemName(subsystem), safeCode,
                safeDetail[0] ? " — " : "", safeDetail);
}

Stats stats() {
  Stats result = {};
  portENTER_CRITICAL(&mux);
  result.count = count;
  result.warnings = warnings;
  result.errors = errors;
  portEXIT_CRITICAL(&mux);
  return result;
}

size_t copyRecent(Entry* destination, size_t capacity, Level minimum, Stats* resultStats) {
  if (!destination || capacity == 0) {
    if (resultStats) *resultStats = stats();
    return 0;
  }

  portENTER_CRITICAL(&mux);
  if (resultStats) {
    resultStats->count = count;
    resultStats->warnings = warnings;
    resultStats->errors = errors;
  }

  size_t selected = 0;
  size_t oldestSelected = count;
  for (size_t offset = 1; offset <= count && selected < capacity; ++offset) {
    const Entry& entry = entries[(head + kCapacity - offset) % kCapacity];
    if (static_cast<uint8_t>(entry.level) >= static_cast<uint8_t>(minimum)) {
      oldestSelected = count - offset;
      ++selected;
    }
  }
  for (size_t i = oldestSelected, output = 0; i < count && output < selected; ++i) {
    const Entry& entry = entries[(head + kCapacity - count + i) % kCapacity];
    if (static_cast<uint8_t>(entry.level) >= static_cast<uint8_t>(minimum)) {
      destination[output++] = entry;
    }
  }
  portEXIT_CRITICAL(&mux);
  return selected;
}

}  // namespace NewoLog
