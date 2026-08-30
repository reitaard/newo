# Neo wake-word research

Research snapshot: 2026-08-30.

## Product naming

- Written product/repository/device identity: **Newo**.
- Spoken assistant name: **Neo**.
- Sherpa contextual hotword: `NEO`.
- Desired local wake phrase: **Neo**.

These are separate layers. `server/config/newo-hotwords.txt` biases Sherpa transcription only after the ESP has already woken and opened `/voice`; it does not configure WakeNet.

## What Newo does today

`NewoAudio::startWakeNet()` starts Arduino-ESP32 `ESP_SR` in `SR_MODE_WAKEWORD`. Newo does not name a WakeNet model or phrase in firmware. The `"MN"` input-format string describes ESP-SR audio channels (microphone plus an unused channel), not the wake phrase.

The actual wake phrase is embedded in the WakeNet model stored in the ESP-SR `model` flash partition. Therefore source code alone cannot prove that the currently flashed board recognizes `Neo`.

## Current upstream ESP-SR findings

Espressif's current ESP-SR supports WakeNet on ESP32-S3. As of August 2026 the upstream project lists WakeNet9/WakeNet9s and the newly released WakeNet10 family. Wake words are model assets, not arbitrary strings passed to the runtime.

The current public/tested model list includes phrases such as `Hi ESP`, `Alexa`, `Jarvis`, `Computer`, and others. No `Neo` WakeNet model is listed in the upstream supported/tested list at the time of this research.

Espressif documents that a WakeNet model can contain up to five wake words and that a wake word usually consists of roughly 3-6 symbols. That guidance does not prove or reject `Neo`; a real `Neo` model still has to be produced and measured for false accepts and missed wakes on Newo's microphone/hardware.

Upstream also notes that WakeNet performance depends heavily on microphone, speaker/cavity design, distance, noise, and the training corpus, so a model that works in a reference setup still needs physical validation on Newo.

## Practical ways to obtain `Neo`

### 1. Recommended first route: Espressif TTS wake-word request

Espressif's public wake-word request program is the lowest-complexity path for Newo. Their issue #88 says TTS-trained wake words are released for community use and that TTS Pipeline V3 supports English. Since August 2024, a new request can qualify by linking an ongoing project or by receiving enough community votes.

Espressif reports its TTS V2 pipeline at about 95-98% of the accuracy of models trained from human samples. This is an upstream comparison, not a guarantee for `Neo` on Newo hardware.

For Newo, request:

```text
Wake word: Neo
Pronunciation: English "Neo" (NEE-oh)
Target: ESP32-S3
Framework: ESP-SR / WakeNet
Project: https://github.com/reitaard/newo
Use: local wake word for the Newo voice assistant
Preferred output: WakeNet model asset that can be packed into srmodels.bin/model partition
```

If Espressif publishes a `Neo` model, use that exact model asset and then rebuild/repack `srmodels.bin` for Newo's `model` partition. Do not change the `/voice`, Sherpa, Qwen, Kokoro, or speaker architecture just to adopt the model.

### 2. Paid/exclusive Espressif customization

Espressif also offers an offline commercial customization service. Their documented customer-corpus route requires at least 20,000 qualified recordings; the documented process normally takes about 2-3 weeks after the corpus is ready and is paid. Espressif can also provide the corpus for an additional fee.

This is unnecessary for the current prototype unless the public TTS-trained `Neo` model is unavailable or its real-device accuracy is inadequate.

### 3. Do not build a DIY training pipeline yet

The public ESP-SR documentation points developers to the Espressif TTS request/community process and the commercial customization service. It does not expose a simple supported local command where Newo can pass the word `Neo` and train a production WakeNet model itself.

For this project, avoid creating a separate wake-word ML training stack unless the official routes fail. That would add complexity without improving the current one-turn chat architecture.

## Model packaging and flashing

ESP-SR loads selected models from a flash partition labeled `model`. Newo uses the Arduino-ESP32 `ESP SR 16M` partition scheme, so a custom `Neo` wake word ultimately means changing the contents of `srmodels.bin`/the `model` partition, not changing a string in `newo_audio.cpp`.

Arduino-ESP32 copies its packaged `srmodels.bin` into the sketch build output when `ESP_SR` is used. On Newo's confirmed Arduino-ESP32 3.3.11 + built-in `esp_sr_16` configuration, a normal Arduino upload has been observed to include the model image at the model-partition offset. `Newo/flash_esp_sr.sh` remains an explicit recovery/manual flashing path; it is not required after every normal upload when the IDE upload already writes `srmodels.bin`.

When a custom `Neo` model becomes available:

1. Keep the existing `esp_sr_16` partition layout unless the new bundle no longer fits.
2. Produce/obtain an `srmodels.bin` that contains the desired WakeNet model.
3. Verify the upload command actually writes the model partition; do not assume an application-only flash updates it.
4. Do not use `Erase All Flash` casually because it also removes NVS/provisioning state and the existing model partition.
5. After flashing, verify the ESP-SR startup/model report over Serial before calling `Neo` supported.

## Acceptance criteria for `Neo`

A `Neo` model is not considered integrated merely because it builds. Physical validation should check:

- repeated wakes at normal speaking volume and multiple distances;
- fast/normal/slow pronunciation of `Neo`;
- missed-wake rate;
- false wakes from ordinary speech, TV/music, and words sounding similar to `Neo`;
- wake while the device is otherwise idle for an extended period;
- WakeNet is suppressed during speaker playback and re-arms afterward;
- after a valid wake, Sherpa transcribes the spoken assistant name as `Neo` using the existing `NEO` contextual bias.

If single-word `Neo` proves too trigger-prone, test a slightly longer phrase such as `Hey Neo` before changing models/frameworks. Do not make that change pre-emptively; measure the single-word target first.

## Current decision

For now:

- keep branding as Newo;
- keep the spoken assistant identity as Neo;
- keep Sherpa hotword bias as `NEO`;
- keep the current local WakeNet architecture unchanged;
- do not claim the installed wake phrase is Neo until a Neo WakeNet model is installed and physically validated;
- pursue Espressif's TTS wake-word request as the preferred low-complexity route.

## Official references

- ESP-SR repository / current WakeNet models: https://github.com/espressif/esp-sr
- WakeNet documentation: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html
- Espressif wake-word customization process: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html
- Model selection/loading and `model` partition: https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/flash_model/README.html
- Espressif TTS/community wake-word request: https://github.com/espressif/esp-sr/issues/88
- Arduino-ESP32 ESP_SR build hook (`srmodels.bin`): https://github.com/espressif/arduino-esp32/blob/master/platform.txt
