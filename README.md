# Newo

Newo is an ESP32-S3 portable assistant platform. Firmware is split into provisioning, connectivity, cloud, and future peripheral layers.

## Hardware and toolchain

- ESP32-S3 N16R8, 16 MB flash, 8 MB OPI PSRAM, 240 MHz
- Arduino-ESP32 3.3.11
- ArduinoJson 7.4.3
- WebSockets 2.7.2
- Adafruit GFX Library 1.12.6
- Adafruit ST7735 and ST7789 Library 1.11.0

Use `ESP32S3 Dev Module`, QIO 80 MHz, 16 MB flash, OPI PSRAM, **`ESP SR 16M (3MB APP/6MB SPIFFS/3.9MB MODEL)`** (`PartitionScheme=esp_sr_16`), and 921600 upload speed.

## Repository

- `Newo/` — Arduino firmware
- `server/` — VPS cloud and Telegram bridge
- `docs/` — architecture and bring-up notes

## Display test

A 240x240 ST7789 display is wired over SPI: SCK GPIO42, MOSI GPIO41, RST GPIO40, DC GPIO38, and CS GPIO2. VCC and BLK connect to 3V3; GND connects to GND. The display initializes in the confirmed physical rotation and renders the Newo face/dashboard UI. Microphone GPIO4/5/6 and RGB GPIO48 remain unchanged.

## Wi-Fi and provisioning

Newo stores up to eight Wi-Fi networks in ESP32 Preferences/NVS. At boot it scans the supported 2.4 GHz band, filters the results to saved SSIDs, ranks visible saved networks by RSSI, and attempts them strongest-first. A disconnected device retries saved networks within bounded recovery windows.

If no saved network exists or none is reachable during the initial 18-second recovery window, Newo starts official Arduino-ESP32 `WiFiProv` over BLE as `PROV_NEWO`. Use the Espressif **ESP BLE Provisioning** app and select Security 1 with no proof-of-possession value. Credentials cross from the provisioning callback through fixed-size buffers and are added or updated in NVS only after `ARDUINO_EVENT_PROV_CRED_SUCCESS`. Existing networks are preserved. Newo then stops BLE and reboots.

BLE provisioning times out after five minutes. Reboot to retry. There is no setup AP, captive portal, local HTTP server, or mDNS service.

Security 1 encrypts the session, but null PoP is a prototype tradeoff. Production provisioning should use a per-device PoP/QR flow and physical gating. Physical BLE and reconnect validation is pending while the board is disconnected.

## Cloud endpoint

- `https://newo.reitaard.de`
- health: `https://newo.reitaard.de/health`
- device channel: `wss://newo.reitaard.de/device`

Caddy proxies the public endpoint to the Node service on loopback `127.0.0.1:8788`. Firmware opens an outbound, certificate-validated WSS connection, authenticates with device ID and bearer secret, sends hello/status telemetry, answers correlated ping/status requests, and acknowledges reboot before restarting.

`Newo/newo_secrets.h` is ignored. Copy `Newo/newo_secrets.example.h` locally and supply the matching device credential and trusted public CA. Missing secrets disable cloud connectivity rather than weakening TLS.

## Local wake-word voice lifecycle

Voice defaults **OFF** so an initial flash is safe without the INMP441 attached. Telegram `/voice` (alias `/v`) toggles OFF to ARMED and toggles ARMED or STREAMING to OFF; `/vs` remains the detailed read-only status alias. In **ARMED**, official Arduino-ESP32 3.3.11 ESP_SR WakeNet listens locally through the shipped English model, sends **no microphone PCM**, and opens no `/voice` socket. `/device` remains the sole persistent cloud WebSocket.

A local wake changes the lifecycle to **STREAMING**: ESP_SR is stopped and releases I2S before the temporary voice task acquires it, opens authenticated certificate-validated `wss://newo.reitaard.de/voice`, and sends direct 20 ms 16 kHz mono PCM16 frames. A final transcript, disconnect, send failure, timeout, or a `/voice` toggle while STREAMING closes `/voice`, releases I2S/task resources, restores WakeNet, and returns to ARMED (or OFF). There is no permanent PCM queue or cloud sender task; transport stalls fail the utterance rather than accumulating stale audio.

Proposed wiring for the generic ESP32-S3 Dev Module (confirm against the physical board schematic before flashing):

```text
INMP441 VDD  -> 3V3
INMP441 GND  -> GND
INMP441 SCK  -> GPIO4  (I2S BCLK)
INMP441 WS   -> GPIO5  (I2S LRCLK/WS)
INMP441 SD   -> GPIO6  (I2S data in)
INMP441 L/R  -> GND    (left channel)
```

GPIO4/5/6 are configurable in `newo_config.h`; the firmware audit found no current Newo assignments for them and deliberately excludes GPIO0 (bootstrap), GPIO19/20 (USB/JTAG), GPIO48 (RGB LED), and flash/PSRAM pins. The module receives 32-bit stereo I2S slots at 16 kHz, selects the configured INMP441 channel, sign-extends the left-aligned 24-bit sample (`slot32 >> 8`), then reduces and clamps it to signed PCM16 (`sample24 >> 8`).

## Display and Telegram control

The ST7789 uses GPIO42 SCK, GPIO41 MOSI, GPIO40 RST, GPIO38 DC, and GPIO2 CS; it is initialized with rotation `3` for the confirmed physical mount. The local display has `IDLE`, `LISTENING`, `THINKING`, `SPEAKING`, `ERROR`, `MESSAGE`, and `ECO` modes. Normal mode draws only a white code-drawn face on black; face status words use bundled Adafruit `FreeSans9pt7b`; compact diagnostic pages use readable `FreeMono9pt7b` rows with `FreeSansBold9pt7b` headings, left aligned at a 16 px margin. One- or two-word messages up to 14 characters use a centered `FreeSans18pt7b` treatment; longer messages word-wrap left aligned. Normal face mode animates only the eye/activity regions at 20 FPS using `millis()` (blinks, gaze/breathing, and mode-specific line/bars/dots/wave/error indicator), dropping to approximately 8.3 FPS during active speaker playback so audio has priority while both eyes and waveform remain animated. Each region is composited in a small 1-bit `GFXcanvas1` before a single bounded TFT blit to avoid animation tearing; text regions remain intact. ECO rotates compact ONLINE, HEALTH, and SERVICES pages every five seconds without blocking.

Authorized Telegram users can use visible `/newo <idle|listening|thinking|speaking|error|short text>` and `/eco`; hidden alias `/n` is equivalent. `/newo` semantic display updates use `display_set`/`display_ack`; `/eco` uses `eco_toggle`/`display_ack`. Status, health, ping, reboot, logs, and errors results briefly mirror compact summaries on-screen before returning to the prior face or ECO dashboard.

The Arduino WebSockets implementation may block one voice-task write for seconds; that task is created only for an utterance, so `/device` and Wi-Fi servicing remain independent. A failed realtime send ends the utterance instead of creating a queue.

## Telegram

Telegram terminates at the VPS:

```text
Telegram -> HTTPS webhook -> VPS -> authenticated WSS -> ESP32-S3
```

The visible bot menu is `/status`, `/health`, `/logs`, `/errors`, and `/reboot`. Short aliases `/s`, `/h`, `/l`, `/e`, and `/r` work but stay hidden; `/ping` and `/p` are hidden developer latency checks. User/chat allowlists remain mandatory. The ESP32 never stores the Telegram token.

Important firmware events still print over USB Serial and are also held in a fixed 64-entry RAM ring buffer. `/health` requests live device counters/memory/reset reason; `/logs` returns recent entries and `/errors` returns warnings/errors. Logs disappear on reboot by design, never enter NVS/filesystem, and are available only through the authenticated device channel and authorized Telegram commands.

`0.3.1-dev` had a physical `loopTask` stack-canary failure when diagnostics copied the full ring buffer onto the 8 KiB Arduino task stack. `0.3.2-dev` keeps the 64-entry global ring but exports selected log entries through temporary PSRAM (with a normal-heap fallback); health copies metadata only.

See [`docs/architecture.md`](docs/architecture.md), [`docs/phase-1.md`](docs/phase-1.md), and [`docs/telegram.md`](docs/telegram.md).

## Build

Speaker output uses a dedicated MAX98357A I2S TX instance: BCLK GPIO21, LRC GPIO47, and DOUT GPIO14. The microphone remains on BCLK GPIO4, WS GPIO5, and SD GPIO6. Kokoro playback is native mono 24 kHz PCM16 LE duplicated to stereo slots with a bounded 24 KiB buffer, 12 KiB / 256 ms audible prebuffer, and 100% digital amplitude. VPS conditioning is one-pass: 110 Hz high-pass, configurable conservative gain (Kokoro default +2 dB), then a 0.95 peak limiter with auto-gain disabled. eSpeak remains an explicitly selectable fallback.

Speaker ON keeps one authenticated `/speaker` WSS connected after `/device` and reuses it with `speaker_begin` / binary PCM / `speaker_end` framing. Kokoro realtime playback uses explicit unknown-length framing (`streaming: true`, bounded `max_bytes`, and exact final `speaker_end.bytes`); known-length framing remains supported for the eSpeak fallback. Speaker OFF stops playback, restores WakeNet/display state, closes the WSS, and releases its dynamic StreamBuffer and TLS/network resources. The mode, volume, and mute settings are persisted in ESP32 NVS; automatic-reply mode is also persisted on the VPS. Manual `/speak` while OFF uses a temporary stream and leaves Speaker OFF.

Open `Newo/Newo.ino`. Build with Arduino-ESP32 3.3.11 and `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,UploadSpeed=921600`. The ESP_SR partition is mandatory because its official model is stored in the model partition. Arduino-ESP32 builds `srmodels.bin` but its ordinary upload recipe does not write it; use `Newo/flash_esp_sr.sh <serial-port>` to flash both the application and the official model partition. Hidden allowlisted Telegram controls wait for correlated `/device` acknowledgements. `/voice`, `/speaker`, and `/eco` are primary toggles with detailed replies; `/volume [0-100]` reads or updates the NVS-persisted runtime speaker gain, `/mute` toggles persisted mute, and `/speak <text>` remains a manual TTS playback command independent of the automatic `/speaker` toggle.

## Repository rule

Never commit Wi-Fi passwords, Telegram tokens, webhook secrets, API keys, private keys, device secrets, VPS credentials, `.env`, or `Newo/newo_secrets.h`. Wi-Fi credentials are provisioned over BLE and stored only in device NVS; Telegram secrets remain on the VPS.
