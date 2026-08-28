# Newo

Newo is an ESP32-S3 portable assistant platform. Firmware is split into provisioning, connectivity, cloud, and future peripheral layers.

## Hardware and toolchain

- ESP32-S3 N16R8, 16 MB flash, 8 MB OPI PSRAM, 240 MHz
- Arduino-ESP32 3.3.11
- ArduinoJson 7.4.3
- WebSockets 2.7.2
- Adafruit GFX Library 1.12.6
- Adafruit ST7735 and ST7789 Library 1.11.0

Use `ESP32S3 Dev Module`, QIO 80 MHz, 16 MB flash, OPI PSRAM, `16M Flash (3MB APP/9.9MB FATFS)`, and 921600 upload speed.

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

## Microphone streaming test

The isolated `newo_audio` module captures an INMP441 and opens a second authenticated, certificate-validated WSS connection to `wss://newo.reitaard.de/voice`. It reuses the device ID and bearer-secret headers from the cloud channel; credentials never enter the URL or serial log.

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

Audio uses a dedicated FreeRTOS capture task and I2S DMA, feeding a 24-frame (480 ms) bounded queue. It keeps 20 ms / 640-byte PCM frames internally but joins exactly five contiguous frames into each 3,200-byte binary WebSocket message (100 ms, approximately 10 sends/sec). A dedicated voice FreeRTOS task exclusively owns the WebSockets client and drains one complete five-frame bundle per healthy iteration; it never sends a partial bundle, padding, framing bytes, or stale data. Queue overflow and reconnect transitions discard stale frames rather than replay speech.

## Display and Telegram control

The ST7789 uses GPIO42 SCK, GPIO41 MOSI, GPIO40 RST, GPIO38 DC, and GPIO2 CS; it is initialized with rotation `3` for the confirmed physical mount. The local display has `IDLE`, `LISTENING`, `THINKING`, `SPEAKING`, `ERROR`, `MESSAGE`, and `ECO` modes. Normal mode draws only a white code-drawn face on black; face status words use bundled Adafruit `FreeSans9pt7b`; compact diagnostic pages use readable `FreeMono9pt7b` rows with `FreeSansBold9pt7b` headings, left aligned at a 16 px margin. One- or two-word messages up to 14 characters use a centered `FreeSans18pt7b` treatment; longer messages word-wrap left aligned. Normal face mode animates only the eye/activity regions at 20 FPS using `millis()` (blinks, gaze/breathing, and mode-specific line/bars/dots/wave/error indicator), compositing each region in a small 1-bit `GFXcanvas1` before a single TFT blit to avoid animation tearing; text regions remain intact. ECO rotates compact ONLINE, HEALTH, and SERVICES pages every five seconds without blocking.

Authorized Telegram users can use visible `/newo <idle|listening|thinking|speaking|error|short text>` and `/eco`; hidden alias `/n` is equivalent. `/newo` semantic display updates use `display_set`/`display_ack`; `/eco` uses `eco_toggle`/`display_ack`. Status, health, ping, reboot, logs, and errors results briefly mirror compact summaries on-screen before returning to the prior face or ECO dashboard.

ArduinoWebsockets 2.7.2 performs synchronous TCP/TLS writes with a five-second library timeout. The dedicated task prevents this from starving the Arduino control loop, but a future transport replacement is required if physical testing still shows multi-second writes; queue growth is never solved by increasing its size.

Voice transport self-heals without disrupting Wi-Fi or `/device`. It enters `DEGRADED` at queue depth 20+, two consecutive sends of 750 ms+, one 1.5 s+ send, or continued drop/overrun growth while saturated. If degradation remains for 2 seconds it closes only `/voice`, flushes all queued audio, clears the bundle, reconnects, and waits 10 seconds before another automatic reset. `voice_reset` on authenticated `/device` (and hidden allowlisted Telegram `/voicereset`) invokes the same routine. The physical investigation motivating this found multi-second synchronous `sendBIN()` stalls, queue saturation, stale/dropped speech, and poor ASR; a fresh voice stream recovered recognition.

`AUDIO_MIC_GAIN` is a fixed physical-test gain, default `4`. It is applied with 64-bit arithmetic to the recovered signed PCM24 sample before PCM16 reduction and saturating clamp. Every five seconds `AUDIO_LEVEL` reports post-gain `p` (peak), `r` (RMS), and cumulative 20 ms-frame `c` (captured), `t` (sent), `d` (dropped), `o` (queue overrun), plus `b` (successful bundle sends), `su` (maximum `sendBIN()` time in microseconds), and `q` (queue high-water mark in frames). `cl=count/percent` is the clipped PCM16 samples and percentage for that reporting interval. Raw samples are never logged.

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

Open `Newo/Newo.ino` as the existing Arduino sketch directory. The current microphone-streaming build was verified with Arduino-ESP32 3.3.11 using `ESP32S3 Dev Module`, QIO 80 MHz, 16 MB flash, OPI PSRAM, `16M Flash (3MB APP/9.9MB FATFS)`, and 921600 upload speed: 1,490,079 bytes flash (47% of 3 MB) and 66,680 bytes static RAM (20% of 320 KB). No hardware was flashed during this build verification.

## Repository rule

Never commit Wi-Fi passwords, Telegram tokens, webhook secrets, API keys, private keys, device secrets, VPS credentials, `.env`, or `Newo/newo_secrets.h`. Wi-Fi credentials are provisioned over BLE and stored only in device NVS; Telegram secrets remain on the VPS.
