# Clock architecture

## Scope and request flow

Clock requests take the short path:

`Sherpa STT -> deterministic clock parser -> validation -> authenticated clock_command -> firmware/NVS -> clock_command_ack -> normalized TTS confirmation`

`server/src/clock-request.js` recognizes current time/date, one-shot alarms, timers, snooze/dismiss, timer pause/resume/cancel/status, and stopwatch start/pause/resume/reset. A request that clearly concerns the clock but omits a required value is rejected with a specific clarification; it is not sent to MiniCPM. Non-clock speech continues through the existing assistant path.

The ESP32 is authoritative. `NewoClockService` validates commands, persists absolute alarms and the recent request-ID cache, maintains timers from monotonic `millis()`, and returns an ACK. The server never says that an alarm or timer was set before that ACK reports `applied: true`.

## Device protocol

Server to device:

```json
{"type":"clock_command","request_id":"uuid","action":"create_alarm","epoch_s":1790035200}
{"type":"clock_command","request_id":"uuid","action":"create_timer","duration_s":300}
{"type":"clock_command","request_id":"uuid","action":"pause_timer"}
{"type":"clock_command","request_id":"uuid","action":"resume_timer"}
{"type":"clock_command","request_id":"uuid","action":"cancel","target":"alarm"}
{"type":"clock_command","request_id":"uuid","action":"dismiss"}
{"type":"clock_command","request_id":"uuid","action":"snooze","duration_s":540}
{"type":"clock_command","request_id":"uuid","action":"status"}
```

Device response:

```json
{"type":"clock_command_ack","request_id":"uuid","applied":true,"duplicate":false,"message":"Timer started."}
```

The firmware retains hashes and outcomes for the eight most recent request IDs in NVS. A retry with the same ID replays the result and cannot create another alarm or timer.

## Scheduling decisions

- Alarms store UTC epoch seconds in NVS. The loop compares them with current system time, so an SNTP correction automatically recalculates when they become due rather than preserving a stale delay.
- Alarms are not accepted until system time is synchronized. A forward clock correction fires an alarm only when it lands within the 60-second due window; older alarms are marked missed and removed.
- Timers store remaining milliseconds and an update point from `millis()`. Wall-clock and timezone changes cannot change countdown duration. Pausing first snapshots the remaining monotonic duration.
- A Nano reset/voice-trigger event dismisses an active alert locally before the ordinary voice path starts.
- Ringing is capped at 10 minutes by the clock service. A full alarm asset cycle is followed by an approximately 500 ms pause before repeating.
- The fixed first-version bounds are eight alarms, four timers, and seven days per timer.

## Alarm audio and filesystem

The repository filesystem source is `Newo/data/audio/newo_alarm.pcm`; it becomes `/audio/newo_alarm.pcm` in the ESP32 SPIFFS partition. The file is raw signed PCM16 little-endian, mono, 24,000 Hz, exactly 1,413,600 bytes (29.45 seconds), SHA-256 `a09f81e8467b279d3c9caababc270552774c92729af55bccf882ff9694baea12`.

`NewoSpeaker` mounts SPIFFS without formatting, validates the exact asset size, and streams 512-byte mono chunks through the existing speaker-output/I²S path. It never loads the full file into RAM. Each chunk is scaled using the separately persisted `alarm-vol` NVS preference, which defaults to 80%, expanded to stereo slots, and written to the established output abstraction. Assistant speaker mute is deliberately not consulted for wake alarms.

An alarm request preempts active network TTS and blocks replacement TTS until ringing stops. Dismiss, snooze, matching cancellation, the local physical trigger, and the 10-minute deadline all request immediate playback termination; the task checks that state at every bounded chunk, drain wait, and 5 ms pause interval. If SPIFFS cannot mount, the file is missing, its size is wrong, it cannot be opened, or reading fails, playback falls back to the existing synthesized two-note chime at the alarm volume.

The selected `esp_sr_16` layout already provides a 6 MiB SPIFFS partition at offset `0x610000`. The raw alarm occupies 1,413,600 bytes, approximately 22.5% of that partition, leaving approximately 4,877,856 bytes before filesystem metadata. The asset is not linked into the application binary.

## Build and tests

Parser tests:

```powershell
cd server
node --test test/clock-request.test.js test/clock-firmware-contract.test.js
```

These tests validate parsing plus the asset byte count/hash, runtime path, bounded streaming contract, repeat/pause lifecycle, synthesized fallback, preemption, immediate-stop wiring, independent persisted volume, ring timeout, and single-owner SNTP setup. They do not prove physical audio or filesystem mounting.

Create the future SPIFFS image after the firmware build directory exists:

```powershell
$Mkspiffs = "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\mkspiffs\0.2.3\mkspiffs.exe"
& $Mkspiffs -c Newo/data -b 4096 -p 256 -s 6291456 build/clock-audit/Newo.spiffs.bin
& $Mkspiffs -l build/clock-audit/Newo.spiffs.bin
```

The listing must contain `/audio/newo_alarm.pcm` with 1,413,600 bytes. The future flash offset is `0x610000`; never write this image to the separate `model` partition at `0xC10000`.

Firmware build (Arduino-ESP32 3.3.10):

```powershell
arduino-cli compile --fqbn "esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=esp_sr_16,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default" --build-path build/clock-audit Newo
```

Prepare the pinned microWakeWord frontend first with Git Bash:

```powershell
& 'C:\Program Files\Git\bin\bash.exe' Newo/prepare_alfred_mww.sh
```

The current branch uses the embedded `Newo/wakeword/alfred.tflite` model. Per `docs/wake-word.md`, do not generate or flash `srmodels.bin`. The `model` partition remains untouched. No flash or VPS deployment is part of this change.

### Future compile and upload sequence

Only after explicit flash authorization:

1. Confirm branch/worktree, COM7 identity, the exact `esp_sr_16` partition table, the source PCM hash, and all expected generated artifacts.
2. Run `Newo/prepare_alfred_mww.sh` with Git Bash.
3. Compile once with the FQBN above into `build/clock-audit`; do not use `compile --upload`.
4. Confirm the application fits the 3,145,728-byte `app0` slot. The last pre-audio clock build was 1,827,058 bytes (58.1%); the asset adds zero application bytes, while the small playback code is expected to keep the result near that value. The exact new size remains a required pre-flash check.
5. Build/list the 6,291,456-byte SPIFFS image using the command above and confirm the image and partition-table offsets.
6. Read/compare the board partition table before writing. With the matching existing layout, preserve bootloader, partition table, NVS, OTA data, and the unused model partition.
7. Write only `build/clock-audit/Newo.ino.bin` at `0x10000` and `build/clock-audit/Newo.spiffs.bin` at `0x610000` to COM7 at 921600 baud, without erase.
8. Reset, open serial, and verify boot, `filesystem=mounted`, `fallback=no`, Alfred readiness, SNTP readiness, restored alarms, and physical ring/dismiss/snooze behavior.

## Research basis

The design follows mature behavior without copying implementation code:

- [Home Assistant intent timers](https://github.com/home-assistant/core/blob/dev/homeassistant/components/intent/timers.py) use monotonic timestamps and snapshot remaining duration on pause.
- [ESPHome Voice PE](https://github.com/esphome/home-assistant-voice-pe/blob/dev/home-assistant-voice.yaml) enables a local stop model only while a timer is ringing; Newo's first local equivalent is the physical reset trigger.
- [OpenVoiceOS alerts](https://github.com/OpenVoiceOS/ovos-skill-alerts) separates alerts, supports snooze and missed-alert handling, and remains useful for recurring-alarm/reminder expansion.
- [Apple's Clock App Intent schema](https://developer.apple.com/documentation/appintents/app-schema-domain-clock) defines explicit typed actions for create/update/delete/snooze/dismiss alarms and create/update/pause/resume/cancel timers.
- [Alexa's Timers API](https://developer.amazon.com/en-US/docs/alexa/smapi/alexa-timers-api-reference.html) exposes explicit create/list/get/pause/resume/cancel operations and preserves remaining duration when paused.
- [ESP-IDF system time](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/system_time.html), [esp_timer](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/esp_timer.html), and [NVS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/nvs_flash.html) support the wall-time, monotonic-time, and persistence split.

## Remaining limitations

- The first version supports one-shot alarms only. The data model has stable IDs and an action boundary suitable for recurrence and reminders, but recurrence rules are not persisted yet.
- Timers intentionally do not survive a restart. Persisting a countdown across power loss needs an explicit product rule for whether powered-off time counts.
- Alarm audio, SPIFFS mounting, physical output priority, volume, and stop latency remain unvalidated on hardware until an explicitly authorized COM7 flash and bounded acceptance test.
- Cancel/pause/resume select the first matching item. Named or ID-based disambiguation and list details are next-step work.
- Natural-language coverage is deliberately narrow. Dates beyond today/tomorrow, weekdays, recurrence, labels, and genuinely complex phrasing should go through a future interpretation fallback whose typed result is revalidated by this deterministic layer.
- No COM7 flash, physical alarm playback test, restart/NVS test on hardware, or VPS deployment has been performed.
