# Newo

Newo is an ESP32-S3 portable assistant platform. Firmware is split into provisioning, connectivity, cloud, and future peripheral layers.

## Hardware and toolchain

- ESP32-S3 N16R8, 16 MB flash, 8 MB OPI PSRAM, 240 MHz
- Arduino-ESP32 3.3.11
- ArduinoJson 7.4.3
- WebSockets 2.7.2

Use `ESP32S3 Dev Module`, QIO 80 MHz, 16 MB flash, OPI PSRAM, `16M Flash (3MB APP/9.9MB FATFS)`, and 921600 upload speed.

## Repository

- `Newo/` — Arduino firmware
- `server/` — VPS cloud and Telegram bridge
- `docs/` — architecture and bring-up notes

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

Open `Newo/Newo.ino` as the existing Arduino sketch directory. With the settings above, firmware `0.3.0-dev` compiles under Arduino-ESP32 3.3.11 at 1,410,663 flash bytes and 49,096 static RAM bytes. Against the supplied pre-BLE baseline (1,192,907 / 50,140), that is +217,756 flash and -1,044 RAM.

## Repository rule

Never commit Wi-Fi passwords, Telegram tokens, webhook secrets, API keys, private keys, device secrets, VPS credentials, `.env`, or `Newo/newo_secrets.h`. Wi-Fi credentials are provisioned over BLE and stored only in device NVS; Telegram secrets remain on the VPS.
