import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { createHash } from "node:crypto";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("alarms persist and recent request outcomes survive restart", async () => {
  const clock = await source("../../Newo/newo_clock_service.cpp");
  assert.match(clock, /preferences_\.getString\(kAlarmsKey/);
  assert.match(clock, /preferences_\.putString\(kAlarmsKey/);
  assert.match(clock, /preferences_\.getBytes\(kRecentKey/);
  assert.match(clock, /preferences_\.putBytes\(kRecentKey/);
  assert.match(clock, /result\.duplicate = true/);
  assert.match(clock, /if \(saveAlarms\(\)\) \{ result\.applied = true/);
});

test("timers use monotonic unsigned deltas and preserve remaining time on pause", async () => {
  const clock = await source("../../Newo/newo_clock_service.cpp");
  assert.match(clock, /const uint32_t elapsed = nowMs - timer\.updatedAtMs/);
  assert.match(clock, /timer\.remainingMs = timerRemaining\(timer, nowMs\)/);
  assert.doesNotMatch(clock, /epochSeconds.*create_timer/);
});

test("alarm due checks use current wall time on every loop and bound missed alerts", async () => {
  const clock = await source("../../Newo/newo_clock_service.cpp");
  assert.match(clock, /void NewoClockService::loop\(time_t wallNow/);
  assert.match(clock, /wallNow\) >= alarm\.epochSeconds/);
  assert.match(clock, /alarm\.epochSeconds \+ 60/);
});

test("physical trigger dismisses ringing before starting the cloud voice path", async () => {
  const sketch = await source("../../Newo/Newo.ino");
  const dismiss = sketch.indexOf("if (newoClock.ringing())");
  const voice = sketch.indexOf("startPhysicalVoiceTrigger()", dismiss);
  assert.ok(dismiss >= 0 && voice > dismiss);
});

test("filesystem alarm asset has the fixed runtime path and exact PCM payload", async () => {
  const asset = await readFile(new URL("../../Newo/data/audio/newo_alarm.pcm", import.meta.url));
  assert.equal(asset.length, 1_413_600);
  assert.equal(asset.length % 2, 0);
  assert.equal(asset.length / (24_000 * 2), 29.45);
  assert.equal(createHash("sha256").update(asset).digest("hex"),
    "a09f81e8467b279d3c9caababc270552774c92729af55bccf882ff9694baea12");
  const speaker = await source("../../Newo/newo_speaker.cpp");
  assert.match(speaker, /kAlarmAssetPath\[\] = "\/audio\/newo_alarm\.pcm"/);
  assert.match(speaker, /SPIFFS\.open\(kAlarmAssetPath, FILE_READ\)/);
  assert.match(speaker, /sizeof\(monoWorking_\)/);
});

test("alarm playback preempts network audio, repeats, falls back, and stops promptly", async () => {
  const [speaker, sketch] = await Promise.all([
    source("../../Newo/newo_speaker.cpp"), source("../../Newo/Newo.ino"),
  ]);
  assert.match(speaker, /fail\("alarm_priority"\)/);
  assert.match(speaker, /if \(alarmRequested_ \|\| alarmActive_ \|\| alarmTask_/);
  assert.match(speaker, /kAlarmRepeatPauseMs = 500/);
  assert.match(speaker, /ALARM_AUDIO_FALLBACK/);
  assert.match(speaker, /source=synth_chime/);
  assert.match(speaker, /while \(alarmRequested_ && !alarmStopRequested_/);
  assert.match(sketch, /newoClock\.dismiss\(\);\s+newoSpeaker\.stopAlarm\(\);/);
});

test("alarm volume is independent, persisted, and defaults to eighty percent", async () => {
  const [storageHeader, storage, speaker] = await Promise.all([
    source("../../Newo/newo_storage.h"), source("../../Newo/newo_storage.cpp"),
    source("../../Newo/newo_speaker.cpp"),
  ]);
  assert.match(storageHeader, /uint8_t alarmVolume_ = 80/);
  assert.match(storage, /kAlarmVolumeKey\[\] = "alarm-vol"/);
  assert.match(storage, /preferences_\.getUChar\(kAlarmVolumeKey, 80\)/);
  assert.match(speaker, /monoWorking_\[i\]\) \* alarmVolume_/);
  assert.doesNotMatch(speaker.match(/void NewoSpeaker::alarmPlaybackTask\(\)[\s\S]*?void NewoSpeaker::loop/)[0], /muted_/);
});

test("SNTP configuration has one owner and clock display only consumes system time", async () => {
  const [timeSource, displayClock] = await Promise.all([
    source("../../Newo/newo_time.cpp"), source("../../Newo/newo_clock.cpp"),
  ]);
  assert.match(timeSource, /configTzTime\(kTimeZone, "pool\.ntp\.org", "time\.nist\.gov"\)/);
  assert.doesNotMatch(displayClock, /configTzTime|configTime|sntp_/);
});
