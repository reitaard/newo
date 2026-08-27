# Newo

Newo is an ESP32-S3 based portable assistant platform. The firmware is being built in phases so networking, provisioning, cloud communication, audio, display, and AI features can evolve independently.

## Current hardware target

- ESP32-S3 N16R8
- 16 MB flash
- 8 MB OPI PSRAM
- 240 MHz dual-core CPU
- Arduino-ESP32 3.3.11
- ArduinoJson 7.4.3

## Repository layout

- `Newo/` — Arduino sketch and ESP32 firmware sources
- `server/` — VPS/cloud + Telegram bridge
- `docs/` — architecture and bring-up notes

Arduino requires the main `.ino` file and its sketch-local `.h/.cpp` files to live in the sketch folder, so the firmware entry point is `Newo/Newo.ino`.

## Phase 1 — foundation

Phase 1 establishes the permanent device foundation:

- device identity: `Newo`
- onboard RGB LED disabled by default
- persistent saved Wi-Fi credentials using ESP32 Preferences/NVS
- automatic connection to the best available saved network using WiFiMulti
- fallback access point: `newo@ai.link`
- random 12-character setup password generated on each setup-mode boot
- captive setup portal for scanning, adding, and removing Wi-Fi networks
- local status API
- mDNS hostname: `newo.local` when the LAN supports local peer access

See [`docs/architecture.md`](docs/architecture.md) and [`docs/phase-1.md`](docs/phase-1.md).

## Cloud endpoint

The Newo cloud service is hosted at:

- `https://newo.reitaard.de`
- health: `https://newo.reitaard.de/health`
- device WebSocket: `wss://newo.reitaard.de/device`

The public endpoint is reverse-proxied by Caddy to the Newo Node service bound only to `127.0.0.1:8788` on the VPS.

## Telegram direction

Newo will connect to the user's Telegram bot through the VPS/cloud layer rather than storing the Telegram bot token on the ESP32.

```text
Telegram -> HTTPS webhook -> VPS/domain -> authenticated Newo cloud channel -> ESP32-S3
```

The VPS will own the Telegram bot token, validate authorized Telegram users/chats, and translate bot commands into Newo device commands. This keeps Telegram-specific networking off the ESP32 and leaves the device free for later real-time microphone, speaker, camera, display, and AI work.

See [`docs/telegram.md`](docs/telegram.md).

## Arduino IDE board settings

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| CPU Frequency | 240MHz (WiFi) |
| Flash Mode | QIO 80MHz |
| Flash Size | 16MB (128Mb) |
| PSRAM | OPI PSRAM |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| Upload Speed | 921600 |
| Core Debug Level | None |

## Dependencies

Built into Arduino-ESP32:

- WiFi
- WiFiMulti
- Preferences
- WebServer
- DNSServer
- ESPmDNS

Additional libraries:

- ArduinoJson 7.4.3
- WebSockets by Markus Sattler 2.7.2

No Telegram-specific Arduino library is required for the primary architecture.

## First upload

Open `Newo/Newo.ino` in Arduino IDE, select the board settings above, compile, and upload. On first boot, Newo has no saved networks and creates a Wi-Fi access point named `newo@ai.link`. Serial Monitor prints a fresh 12-character setup password and the setup URL. Connect using that password and open `http://192.168.4.1` if the captive portal does not appear automatically.

> The setup link is local HTTP inside the password-protected setup WLAN. Phase 1 is still development firmware; provisioning and credential reset will receive additional physical/authentication hardening before production use.

## Bring-up note

If Arduino IDE offers to move `Newo.ino` into a `Newo/` directory, use the repository's existing `Newo/Newo.ino` instead. The project is already laid out as a valid Arduino sketch folder; moving only the `.ino` away from its local headers causes `newo_config.h: No such file or directory`.

## Repository rule

Do not commit real Wi-Fi passwords, Telegram bot tokens, webhook secrets, API keys, certificates, or VPS credentials. Network passwords are entered through Newo's setup portal and stored on-device in NVS. Telegram secrets stay on the VPS.
