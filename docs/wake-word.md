# Hands-free wake and owner voiceprint

Research and implementation snapshot: 2026-09-13.

Newo uses Espressif's published WakeNet9 `wn9_hiwalle_tts2` model for the local phrase **Hi Wall-E**. WakeNet only opens the existing authenticated `/voice` stream; it does not create another microphone, ASR, assistant, TTS, or speaker path.

```text
OFF -- voice on --> ARMED
ARMED -- Hi Wall-E --> STREAMING
STREAMING -- final/error/timeout --> ARMED
ARMED -- speaker playback --> ARMED (WakeNet released/suppressed)
STREAMING -- /v --> OFF
OFF/ARMED -- /v --> STREAMING --> OFF
```

`ESP_SR.end()` stops its feed/detect tasks and releases microphone I2S before the direct streaming task starts. Speaker playback suppresses WakeNet for the whole playback and re-arms it after completion. The manual `/v` one-shot remains independent and intentionally settles OFF.

Physical barge-in is explicitly disabled in this version. The microphone and speaker are mutually exclusive at the firmware lifecycle boundary: WakeNet is stopped during playback and `/v` is rejected while playback owns I2S. The shared server generation-cancellation logic remains valid for reachable new streams, but tests do not make a simultaneous microphone path exist. No Nano trigger or fake interruption claim is used.

## The model partition is mandatory

Firmware source cannot select an absent WakeNet model. Arduino-ESP32 3.3.10's installed ESP32-S3 SDK currently selects `CONFIG_SR_WN_WN9_HIESP=y`, so its stock `srmodels.bin` contains **Hi ESP**, not Hi Wall-E. The `esp_sr_16` board option copies that stock file into the build and uploads it at `0xC10000`.

Use `Newo/prepare_hiwalle_srmodels.ps1` after compiling and before uploading. It clones the installed S3 sdkconfig, changes only the two WakeNet selections, and invokes ESP-SR's official `model/movemodel.py`. The script refuses anything except ESP-SR component `2.4.6`, the exact version recorded in Arduino-ESP32 3.3.10's installed `versions.txt`. The generated `srmodels.bin` replaces the build output that the Arduino post-build hook populated.

```powershell
# In an activated ESP-IDF shell, resolve the pinned registry component once:
mkdir C:\src\newo-sr-pack; cd C:\src\newo-sr-pack
idf.py create-project model_pack
cd model_pack
idf.py add-dependency "espressif/esp-sr==2.4.6"
idf.py reconfigure
cd C:\Users\re_Lax\Desktop\re\newo
arduino-cli compile --fqbn "esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=esp_sr_16,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default" --output-dir .\Newo\build\esp32.esp32.esp32s3 .\Newo
.\Newo\prepare_hiwalle_srmodels.ps1 -EspSrPath C:\src\newo-sr-pack\model_pack\managed_components\espressif__esp-sr
arduino-cli upload -p COM7 --fqbn "esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=esp_sr_16,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default" --input-dir .\Newo\build\esp32.esp32.esp32s3
```

Never use erase-all for this workflow. Physical acceptance must confirm `WAKENET_ARMED`, repeated wake detection, release before streaming, suppression during playback, and re-arm after playback.

## Owner enrollment and verification

WakeNet is phrase detection, not speaker identity. `/owner_enroll` starts a separate three-sample enrollment. For each sample use `/v` and say only “Hi Wall-E”; `/owner_status` shows progress and `/owner_cancel` aborts. A mismatched transcript does not consume a sample.

The server's dedicated speaker worker uses the existing native `sherpa-onnx-node` binding with the English VoxCeleb 3D-Speaker CAM++ ONNX model. Each accepted sample produces a normalized embedding. Three embeddings are averaged and normalized into `data/voiceprints/<device>-owner.json`, written atomically with owner-only permissions. This data is separate from the WakeNet model partition and runtime assistant profile state.

For ordinary turns microphone PCM is sent concurrently to the ASR worker and speaker worker. ASR final starts the assistant immediately; speaker scoring completes independently and logs `SPEAKER_IDENTITY` with `identity`, cosine `score`, and configured `threshold`. Version one reports `owner` or `unknown` and never blocks conversation. The result boundary can later gate personal memory or device actions without changing the audio pipeline.

```bash
cd /opt/newo/server
mkdir -p models data/voiceprints
curl -fL -o models/3dspeaker_speech_campplus_sv_en_voxceleb_16k.onnx \
  https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/3dspeaker_speech_campplus_sv_en_voxceleb_16k.onnx
chmod 700 data/voiceprints
```

CAM++ is preferred here over a separate Python 3D-Speaker service because Newo already ships the maintained Sherpa native Node binding and its speaker extractor API. A dedicated worker preserves parallelism with ASR while avoiding another interpreter, package stack, port, proxy, and service. The underlying embedding model is still 3D-Speaker CAM++.

Official references:

- https://github.com/espressif/esp-sr
- https://github.com/espressif/esp-sr/blob/master/wakeword_list.md
- https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html
- https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/flash_model/README.html
- https://k2-fsa.github.io/sherpa/onnx/javascript-api/index.html
- https://github.com/modelscope/3D-Speaker
- https://github.com/xinnan-tech/xiaozhi-esp32-server
