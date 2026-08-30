# Neo wake-word research

Research snapshot: 2026-08-30.

## Project status

**Wake-word work is deferred.** Newo will not train, request, integrate, or flash a custom `Neo` wake-word model in the current milestone.

The current priority is to finish and physically validate the one-turn chat path using the existing Telegram `/voice` command (alias `/v`) as the manual trigger. Wake-word work remains a future replacement for that manual trigger; it must not change the `/voice` → Sherpa → Qwen → Kokoro → `/speaker` architecture.

## Product naming

- Written product/repository/device identity: **Newo**.
- Spoken assistant name: **Neo**.
- Sherpa contextual hotword: `NEO`.
- Desired future local wake phrase: **Neo**.

These are separate layers. `server/config/newo-hotwords.txt` biases Sherpa transcription only after the ESP has opened `/voice`; it does not configure WakeNet.

## What Newo does today

`NewoAudio::startWakeNet()` starts Arduino-ESP32 `ESP_SR` in `SR_MODE_WAKEWORD`. Newo does not name a WakeNet model or phrase in firmware. The `"MN"` input-format string describes ESP-SR audio channels (microphone plus an unused channel), not the wake phrase.

The actual wake phrase is embedded in the WakeNet model stored in the ESP-SR `model` flash partition. Therefore source code alone cannot prove that the currently flashed board recognizes `Neo`.

The existing production firmware still contains the OFF/ARMED/STREAMING WakeNet lifecycle. Until the manual-toggle change is implemented, `/voice` toggles OFF to ARMED and WakeNet is still the trigger that advances ARMED to STREAMING. The manual-trigger milestone below is a planned change, not a statement about the currently deployed firmware.

## Current upstream ESP-SR findings

Espressif's current ESP-SR supports WakeNet on ESP32-S3. As of August 2026 the upstream project lists WakeNet9/WakeNet9s and the newly released WakeNet10 family. Wake words are model assets, not arbitrary strings passed to the runtime.

The current public/tested model list includes phrases such as `Hi ESP`, `Alexa`, `Jarvis`, `Computer`, and others. No `Neo` WakeNet model is listed in the upstream supported/tested list at the time of this research.

Espressif documents that a WakeNet model can contain up to five wake words and that a wake word usually consists of roughly 3-6 symbols. That guidance does not prove or reject `Neo`; a real `Neo` model still has to be produced and measured for false accepts and missed wakes on Newo's microphone/hardware.

Upstream also notes that WakeNet performance depends heavily on microphone, speaker/cavity design, distance, noise, and the training corpus, so a model that works in a reference setup still needs physical validation on Newo.

## Future ways to obtain `Neo`

These are research options only. None is active work for the current manual-toggle milestone.

### Own/custom Neo model

The preferred long-term direction is to own the `Neo` wake-word behavior rather than make the current assistant depend permanently on a vendor-provided phrase. The exact training/runtime framework should be selected later, after the manual chat path is physically stable and resource measurements on the ESP32-S3 are available.

Future work should keep the wake detector isolated behind the same trigger boundary: successful local detection should only request the already-existing voice stream. It must not redesign Sherpa, Qwen, Kokoro, `/speaker`, or device/cloud authentication.

### Espressif TTS wake-word request

Espressif's public wake-word request program remains a fallback/reference option. Their issue #88 says TTS-trained wake words are released for community use and that TTS Pipeline V3 supports English. Since August 2024, a new request can qualify by linking an ongoing project or by receiving enough community votes.

A possible request would be:

```text
Wake word: Neo
Pronunciation: English "Neo" (NEE-oh)
Target: ESP32-S3
Framework: ESP-SR / WakeNet
Project: https://github.com/reitaard/newo
Use: local wake word for the Newo voice assistant
Preferred output: WakeNet model asset that can be packed into srmodels.bin/model partition
```

This is not the current plan and should not be submitted unless the project later chooses the Espressif route.

### Paid/exclusive Espressif customization

Espressif also offers an offline commercial customization service. Their documented customer-corpus route requires at least 20,000 qualified recordings; the documented process normally takes about 2-3 weeks after the corpus is ready and is paid. Espressif can also provide the corpus for an additional fee.

This is unnecessary for the current prototype.

## Planned manual-toggle milestone

The immediate goal is a simple one-turn chat flow with **no wake-word dependency**.

Target behavior for Telegram `/voice` and `/v`:

```text
OFF
  -- /v --> STREAMING
STREAMING
  -- /v --> OFF (cancel)
STREAMING
  -- final/error/timeout/disconnect --> OFF
```

The intended path is:

```text
/v
  -> acquire microphone/I2S
  -> open authenticated temporary /voice
  -> stream existing 20 ms PCM16 frames
  -> Sherpa final transcript
  -> Qwen one-turn reply
  -> existing Kokoro + /speaker playback
  -> remain OFF after the turn
```

Implementation constraints for that milestone:

- `/v` should start one microphone session directly; it should not require a WakeNet event.
- A second `/v` while STREAMING should cancel the current session and settle to OFF.
- After a final transcript, failure, timeout, or disconnect, voice should settle to OFF instead of re-arming the current unknown WakeNet model.
- Keep the current WakeNet/ARMED code available but dormant for future wake-word work rather than deleting it.
- Do not add a fourth voice state. OFF and STREAMING are sufficient for manual operation; ARMED remains reserved for local wake-word mode.
- Do not add another audio queue, WebSocket, ASR path, LLM path, or TTS path.
- Preserve the existing direct 20 ms microphone frames, Sherpa worker/backpressure, finalized-transcript deduplication, bounded one-turn Qwen runtime, Kokoro streaming, Opus speaker path, and playback suppression.
- The Telegram acknowledgement for starting a manual session should return promptly once STREAMING has started; it must not be held until the entire utterance finishes.
- `/vs` should make the active trigger clear. During the manual milestone it should not imply that `Neo` WakeNet is ready; it should show the voice state and identify the trigger as manual/Telegram, while wake-word support is deferred.
- No tool/function calling or conversation memory is part of this milestone.

### Firmware integration shape

The cleanest change is to add a bounded one-shot/manual-start path in `NewoAudio` rather than pretending the unknown WakeNet model detected a wake.

The manual path should start the existing streaming task directly from OFF and remember that the session must return to OFF. The existing real WakeNet path can continue to mark future wake-triggered sessions as re-armable. This keeps one streaming implementation and avoids duplicating I2S or `/voice` ownership.

The current `transitionPending_`/voice-ack handling also needs review: today it is useful for waiting for a STREAMING cancellation to fully settle, but a manual `/v` start must not defer its Telegram acknowledgement until the stream ends.

## Future model packaging and flashing

If Newo later returns to ESP-SR/WakeNet for a custom `Neo` model, ESP-SR loads selected models from a flash partition labeled `model`. Newo uses the Arduino-ESP32 `ESP SR 16M` partition scheme, so a custom WakeNet `Neo` asset would ultimately change the contents of `srmodels.bin`/the `model` partition, not a string in `newo_audio.cpp`.

Arduino-ESP32 copies its packaged `srmodels.bin` into the sketch build output when `ESP_SR` is used. On Newo's confirmed Arduino-ESP32 3.3.11 + built-in `esp_sr_16` configuration, a normal Arduino upload has been observed to include the model image at the model-partition offset. `Newo/flash_esp_sr.sh` remains an explicit recovery/manual flashing path.

Do not use `Erase All Flash` casually because it also removes NVS/provisioning state and the existing model partition.

## Future acceptance criteria for `Neo`

When custom wake-word work resumes, a `Neo` model is not considered integrated merely because it builds. Physical validation should check repeated wakes, pronunciation variation, missed-wake rate, false wakes from ordinary speech/TV/music/similar words, long idle operation, speaker-playback suppression, re-arming after playback, and Sherpa transcription of the spoken assistant name as `Neo`.

If single-word `Neo` proves too trigger-prone, `Hey Neo` is a possible fallback to test later. Do not change the desired phrase pre-emptively; measure `Neo` first.

## Current decision

For now:

- keep branding as Newo;
- keep the spoken assistant identity as Neo;
- keep Sherpa hotword bias as `NEO`;
- defer all custom wake-word training/integration/flashing;
- keep existing WakeNet code only as future infrastructure;
- make `/voice` / `/v` the manual one-turn listen/cancel trigger for the next implementation milestone;
- keep the assistant chat-only with no tool calling;
- do not claim the installed WakeNet phrase is Neo.

## Official references

- ESP-SR repository / current WakeNet models: https://github.com/espressif/esp-sr
- WakeNet documentation: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html
- Espressif wake-word customization process: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html
- Model selection/loading and `model` partition: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/flash_model/README.html
- Espressif TTS/community wake-word request: https://github.com/espressif/esp-sr/issues/88
- Arduino-ESP32 ESP_SR build hook (`srmodels.bin`): https://github.com/espressif/arduino-esp32/blob/master/platform.txt
