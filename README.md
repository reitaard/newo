# Newo

Newo is an ESP32-S3 based portable assistant platform. The firmware is being built in phases so networking, provisioning, cloud communication, audio, display, and AI features can evolve independently.

## Current hardware target

- ESP32-S3 N16R8
- 16 MB flash
- 8 MB OPI PSRAM
- 240 MHz dual-core CPU
- Arduino-ESP32 3.3.11
- ArduinoJson 7.4.3

## Phase 1 — foundation

Phase 1 establishes the permanent device foundation:

- device identity: `Newo`
- onboard RGB LED disabled by default
- persistent saved Wi-Fi credentials using ESP32 Preferences/NVS
- automatic connection to the best available saved network using WiFiMulti
- fallback access point: `Newo-Setup`
- random 12-character setup password generated on each setup-mode boot
- captive setup portal for scanning, adding, and removing Wi-Fi networks
- local status API
- mDNS hostname: `newo.local` when the LAN supports local peer access

See [`docs/architecture.md`](docs/architecture.md) and [`docs/phase-1.md`](docs/phase-1.md).

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

Additional library:

- ArduinoJson 7.4.3

## First upload

Open `Newo.ino` in Arduino IDE, select the board settings above, compile, and upload. On first boot, Newo has no saved networks and creates a Wi-Fi access point named `Newo-Setup`. Serial Monitor prints a fresh 12-character setup password and the setup URL. Connect using that password and open `http://192.168.4.1` if the captive portal does not appear automatically.

> The setup link is local HTTP inside the password-protected setup WLAN. Phase 1 is still development firmware; provisioning and credential reset will receive additional physical/authentication hardening before production use.

## Repository rule

Do not commit real Wi-Fi passwords, API keys, certificates, or VPS credentials. Network passwords are entered through Newo's setup portal and stored on-device in NVS.
