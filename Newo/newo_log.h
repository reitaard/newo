#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

namespace NewoLog {

enum class Level : uint8_t { INFO, WARN, ERROR };
enum class Subsystem : uint8_t { BOOT, SYSTEM, STORAGE, WIFI, PROV, CLOUD };

constexpr size_t kCapacity = 64;
constexpr size_t kDetailSize = 97;

struct Entry {
  uint32_t sequence;
  uint32_t firstMs;
  uint32_t lastMs;
  uint16_t repeat;
  Level level;
  Subsystem subsystem;
  char code[32];
  char detail[kDetailSize];
};

struct Stats {
  size_t count;
  uint32_t warnings;
  uint32_t errors;
};

static_assert(sizeof(Entry) == 148, "Entry size changed; review RAM budget");
static_assert(sizeof(Stats) == 12, "Stats must remain small");

void log(Level level, Subsystem subsystem, const char* code, const char* detail = nullptr);
Stats stats();
// Copies up to capacity newest matching entries in chronological order. The caller
// owns destination storage and must keep allocation/serialization outside the lock.
size_t copyRecent(Entry* destination, size_t capacity, Level minimum, Stats* resultStats = nullptr);
const char* levelName(Level level);
const char* subsystemName(Subsystem subsystem);

}  // namespace NewoLog
