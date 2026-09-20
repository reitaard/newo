# Hands-free wake and owner voiceprint

Current wake implementation: Alfred microWakeWord.

Newo uses the embedded `Newo/wakeword/alfred.tflite` model as its only local wake detector. The former ESP-SR / Hi Wall-E backend and its model-preparation/flash scripts have been removed.

```text
OFF -- voice on --> ARMED
ARMED -- Alfred --> STREAMING
STREAMING -- final/error/timeout --> ARMED
ARMED -- speaker playback --> ARMED (wake detector released/suppressed)
STREAMING -- /v --> OFF
OFF/ARMED -- /v --> STREAMING --> OFF
```

The wake detector owns microphone I2S only while ARMED. Before a direct voice stream begins it stops and releases I2S; speaker playback suppresses local wake detection and the detector re-arms after the assistant turn completes. `/v` remains an independent manual one-shot.

## Alfred runtime

The model is compiled into the application binary; there is no separate wake-model flash step.

- model: `Newo/wakeword/alfred.tflite`
- sample rate: 16 kHz PCM16
- frontend: 30 ms window, 10 ms step, 40 channels, 125-7500 Hz
- input: `[1,2,40]` int8
- output: `[1,1]` uint8
- threshold: 0.95
- sliding window: 5
- tensor arena: 192 KiB initial allocation, preferably PSRAM
- streaming state: TFLite Micro resource variables remain alive while ARMED

`Newo/prepare_alfred_mww.sh` prepares the pinned TFLM microfrontend sources used by the Arduino build. Run it before compiling a fresh checkout:

```bash
bash Newo/prepare_alfred_mww.sh
```

Then compile/upload the application normally. Do not generate or flash `srmodels.bin` for Alfred.

## Physical acceptance

After flashing, serial should reach `MWW_MODEL_READY` and `MWW_ARMED`. Saying **Alfred** should emit `MWW_WAKE`, hand microphone ownership to the existing `/voice` streaming path, complete the assistant turn, and then re-arm Alfred. Validate repeated wake cycles, normal/quiet speech, distance, background audio, playback suppression, and false wakes before merging the branch to production.

## Owner enrollment and verification

Owner voiceprint is separate from wake-word recognition. `/owner_enroll`, `/owner_status`, and `/owner_cancel` remain server-side voiceprint controls; enrollment samples should use the assistant's current wake/identity phrase guidance rather than depending on a wake-model partition.
