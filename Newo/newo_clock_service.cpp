#include "newo_clock_service.h"

#include <ArduinoJson.h>
#include <cstring>

namespace {
constexpr char kNamespace[] = "newo-clock";
constexpr char kAlarmsKey[] = "alarms-v1";
constexpr char kRecentKey[] = "recent-v1";
constexpr uint64_t kValidEpoch = 1'700'000'000ULL;
}

bool NewoClockService::begin() {
  if (started_) return true;
  if (!preferences_.begin(kNamespace, false)) return false;
  started_ = true;
  const size_t recentBytes = preferences_.getBytesLength(kRecentKey);
  if (recentBytes == sizeof(recent_)) preferences_.getBytes(kRecentKey, recent_, sizeof(recent_));
  return loadAlarms();
}

uint32_t NewoClockService::requestHash(const char* value) {
  uint32_t hash = 2166136261U;
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(value); p && *p; ++p) hash = (hash ^ *p) * 16777619U;
  return hash;
}

uint32_t NewoClockService::timerRemaining(const Timer& timer, uint32_t nowMs) {
  if (!timer.active || timer.paused) return timer.remainingMs;
  const uint32_t elapsed = nowMs - timer.updatedAtMs;
  return elapsed >= timer.remainingMs ? 0 : timer.remainingMs - elapsed;
}

bool NewoClockService::loadAlarms() {
  const String raw = preferences_.getString(kAlarmsKey, "[]");
  JsonDocument doc;
  if (deserializeJson(doc, raw) || !doc.is<JsonArray>()) return false;
  uint8_t index = 0;
  for (JsonObject item : doc.as<JsonArray>()) {
    if (index >= kMaxAlarms) break;
    const uint64_t epoch = item["epoch_s"] | 0ULL;
    const uint32_t id = item["id"] | 0U;
    if (epoch < kValidEpoch || id == 0) continue;
    alarms_[index++] = { id, epoch, true };
    if (id >= nextId_) nextId_ = id + 1;
  }
  return true;
}

bool NewoClockService::saveAlarms() {
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (const Alarm& alarm : alarms_) if (alarm.active) {
    JsonObject item = array.add<JsonObject>();
    item["id"] = alarm.id;
    item["epoch_s"] = alarm.epochSeconds;
  }
  String raw;
  serializeJson(doc, raw);
  return preferences_.putString(kAlarmsKey, raw) > 0;
}

void NewoClockService::remember(uint32_t hash, const Result& result) {
  recent_[recentNext_] = { hash, result };
  recentNext_ = (recentNext_ + 1) % kRecentRequests;
  preferences_.putBytes(kRecentKey, recent_, sizeof(recent_));
}

NewoClockService::Result NewoClockService::execute(const Command& command, time_t wallNow, uint32_t nowMs) {
  Result result;
  if (!started_ || !command.requestId[0] || !command.action[0]) { strlcpy(result.error, "invalid_request", sizeof(result.error)); return result; }
  const uint32_t hash = requestHash(command.requestId);
  for (const Recent& recent : recent_) if (recent.hash == hash) {
    result = recent.result;
    result.duplicate = true;
    return result;
  }

  if (strcmp(command.action, "create_alarm") == 0) {
    if (wallNow < static_cast<time_t>(kValidEpoch)) strlcpy(result.error, "time_unsynchronized", sizeof(result.error));
    else if (command.epochSeconds <= static_cast<uint64_t>(wallNow)) strlcpy(result.error, "time_in_past", sizeof(result.error));
    else {
      Alarm* slot = nullptr;
      for (Alarm& alarm : alarms_) if (!alarm.active) { slot = &alarm; break; }
      if (!slot) strlcpy(result.error, "alarm_limit", sizeof(result.error));
      else {
        *slot = { nextId_++, command.epochSeconds, true };
        if (saveAlarms()) { result.applied = true; snprintf(result.message, sizeof(result.message), "Alarm %lu set.", static_cast<unsigned long>(slot->id)); }
        else { slot->active = false; strlcpy(result.error, "persistence_failed", sizeof(result.error)); }
      }
    }
  } else if (strcmp(command.action, "create_timer") == 0) {
    if (command.durationSeconds == 0 || command.durationSeconds > 7U * 24U * 3600U) strlcpy(result.error, "invalid_duration", sizeof(result.error));
    else {
      Timer* slot = nullptr;
      for (Timer& timer : timers_) if (!timer.active) { slot = &timer; break; }
      if (!slot) strlcpy(result.error, "timer_limit", sizeof(result.error));
      else { *slot = { nextId_++, command.durationSeconds * 1000U, nowMs, true, false }; result.applied = true; strlcpy(result.message, "Timer started.", sizeof(result.message)); }
    }
  } else if (strcmp(command.action, "dismiss") == 0) {
    result.applied = dismiss();
    if (!result.applied) strlcpy(result.error, "not_ringing", sizeof(result.error));
    else strlcpy(result.message, "Dismissed.", sizeof(result.message));
  } else if (strcmp(command.action, "snooze") == 0) {
    if (!ringing_ || !ringingAlarm_ || command.durationSeconds == 0) strlcpy(result.error, "not_found", sizeof(result.error));
    else {
      for (Alarm& alarm : alarms_) if (alarm.id == ringingId_) { alarm.epochSeconds = static_cast<uint64_t>(wallNow) + command.durationSeconds; alarm.active = true; break; }
      ringing_ = false;
      result.applied = saveAlarms();
      if (!result.applied) strlcpy(result.error, "persistence_failed", sizeof(result.error));
      else strlcpy(result.message, "Snoozed.", sizeof(result.message));
    }
  } else if (strcmp(command.action, "pause_timer") == 0 || strcmp(command.action, "resume_timer") == 0) {
    const bool pause = command.action[0] == 'p';
    for (Timer& timer : timers_) if (timer.active && timer.paused != pause) {
      timer.remainingMs = timerRemaining(timer, nowMs); timer.updatedAtMs = nowMs; timer.paused = pause; result.applied = true; break;
    }
    if (!result.applied) strlcpy(result.error, "not_found", sizeof(result.error));
    else strlcpy(result.message, pause ? "Timer paused." : "Timer resumed.", sizeof(result.message));
  } else if (strcmp(command.action, "cancel") == 0) {
    const bool targetIsAlarm = strcmp(command.target, "alarm") == 0;
    if (ringing_ && ringingAlarm_ == targetIsAlarm) result.applied = dismiss();
    if (!result.applied && targetIsAlarm) {
      for (Alarm& alarm : alarms_) if (alarm.active) { alarm.active = false; result.applied = saveAlarms(); break; }
    } else if (!result.applied) for (Timer& timer : timers_) if (timer.active) { timer.active = false; result.applied = true; break; }
    if (!result.applied) strlcpy(result.error, "not_found", sizeof(result.error));
    else strlcpy(result.message, "Cancelled.", sizeof(result.message));
  } else if (strstr(command.action, "stopwatch")) {
    if (strcmp(command.action, "start_stopwatch") == 0) { stopwatchElapsedMs_ = 0; stopwatchUpdatedAtMs_ = nowMs; stopwatchRunning_ = true; result.applied = true; }
    else if (strcmp(command.action, "pause_stopwatch") == 0 && stopwatchRunning_) { stopwatchElapsedMs_ += nowMs - stopwatchUpdatedAtMs_; stopwatchRunning_ = false; result.applied = true; }
    else if (strcmp(command.action, "resume_stopwatch") == 0 && !stopwatchRunning_) { stopwatchUpdatedAtMs_ = nowMs; stopwatchRunning_ = true; result.applied = true; }
    else if (strcmp(command.action, "reset_stopwatch") == 0) { stopwatchElapsedMs_ = 0; stopwatchRunning_ = false; result.applied = true; }
    if (!result.applied) strlcpy(result.error, "invalid_state", sizeof(result.error));
    else strlcpy(result.message, "Stopwatch updated.", sizeof(result.message));
  } else if (strcmp(command.action, "status") == 0) {
    result.applied = true;
    summarize(result, wallNow, nowMs);
  } else strlcpy(result.error, "unsupported_action", sizeof(result.error));

  remember(hash, result);
  return result;
}

void NewoClockService::summarize(Result& result, time_t, uint32_t nowMs) const {
  unsigned alarms = 0, timers = 0;
  uint32_t nextTimer = UINT32_MAX;
  for (const Alarm& alarm : alarms_) if (alarm.active) ++alarms;
  for (const Timer& timer : timers_) if (timer.active) { ++timers; const uint32_t left = timerRemaining(timer, nowMs); if (left < nextTimer) nextTimer = left; }
  if (!alarms && !timers && !stopwatchRunning_ && stopwatchElapsedMs_ == 0) strlcpy(result.summary, "There are no active alarms or timers.", sizeof(result.summary));
  else snprintf(result.summary, sizeof(result.summary), "%u alarm%s and %u timer%s active.%s", alarms, alarms == 1 ? "" : "s", timers, timers == 1 ? "" : "s", stopwatchRunning_ ? " Stopwatch running." : "");
}

void NewoClockService::loop(time_t wallNow, uint32_t nowMs) {
  if (ringing_) {
    if (nowMs - ringingStartedMs_ >= kMaxRingDurationMs) dismiss();
    return;
  }
  if (wallNow >= static_cast<time_t>(kValidEpoch)) for (Alarm& alarm : alarms_) if (alarm.active && static_cast<uint64_t>(wallNow) >= alarm.epochSeconds) {
    alarm.active = false;
    if (static_cast<uint64_t>(wallNow) <= alarm.epochSeconds + 60) { ringing_ = true; ringingAlarm_ = true; ringingId_ = alarm.id; ringingStartedMs_ = nowMs; }
    saveAlarms();
    if (ringing_) return;
  }
  for (Timer& timer : timers_) if (timer.active && !timer.paused && timerRemaining(timer, nowMs) == 0) {
    timer.active = false; ringing_ = true; ringingAlarm_ = false; ringingId_ = timer.id; ringingStartedMs_ = nowMs; return;
  }
}

bool NewoClockService::dismiss() {
  if (!ringing_) return false;
  ringing_ = false;
  ringingAlarm_ = false;
  ringingId_ = 0;
  ringingStartedMs_ = 0;
  return true;
}
