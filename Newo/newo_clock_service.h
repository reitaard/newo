#pragma once

#include <Arduino.h>
#include <Preferences.h>

class NewoClockService {
 public:
  static constexpr uint32_t kMaxRingDurationMs = 10U * 60U * 1000U;
  struct Command {
    char requestId[40] = {};
    char action[24] = {};
    char target[12] = {};
    uint64_t epochSeconds = 0;
    uint32_t durationSeconds = 0;
  };
  struct Result {
    bool applied = false;
    bool duplicate = false;
    char error[24] = {};
    char message[80] = {};
    char summary[128] = {};
  };

  bool begin();
  Result execute(const Command& command, time_t wallNow, uint32_t monotonicNowMs);
  void loop(time_t wallNow, uint32_t monotonicNowMs);
  bool ringing() const { return ringing_; }
  const char* ringingLabel() const { return ringingAlarm_ ? "Alarm ringing" : "Timer ringing"; }
  bool dismiss();

 private:
  static constexpr uint8_t kMaxAlarms = 8;
  static constexpr uint8_t kMaxTimers = 4;
  static constexpr uint8_t kRecentRequests = 8;
  struct Alarm { uint32_t id = 0; uint64_t epochSeconds = 0; bool active = false; };
  struct Timer { uint32_t id = 0; uint32_t remainingMs = 0; uint32_t updatedAtMs = 0; bool active = false; bool paused = false; };
  struct Recent { uint32_t hash = 0; Result result = {}; };

  bool loadAlarms();
  bool saveAlarms();
  void remember(uint32_t hash, const Result& result);
  static uint32_t requestHash(const char* value);
  static uint32_t timerRemaining(const Timer& timer, uint32_t nowMs);
  void summarize(Result& result, time_t wallNow, uint32_t monotonicNowMs) const;

  Preferences preferences_;
  Alarm alarms_[kMaxAlarms] = {};
  Timer timers_[kMaxTimers] = {};
  Recent recent_[kRecentRequests] = {};
  uint8_t recentNext_ = 0;
  uint32_t nextId_ = 1;
  bool started_ = false;
  bool ringing_ = false;
  bool ringingAlarm_ = false;
  uint32_t ringingId_ = 0;
  uint32_t ringingStartedMs_ = 0;
  uint32_t stopwatchElapsedMs_ = 0;
  uint32_t stopwatchUpdatedAtMs_ = 0;
  bool stopwatchRunning_ = false;
};
