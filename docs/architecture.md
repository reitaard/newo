# Newo architecture

## Goal

Newo is a portable ESP32-S3 assistant endpoint. The ESP32 handles device I/O, local provisioning, connectivity, and real-time peripheral work; larger AI inference can live on a VPS, home GPU, or other backend.

The firmware is split into small modules so later audio, display, AI, cloud, Telegram, and OTA work does not turn the main sketch into one large file.

## Firmware layers

```text
Newo.ino
  |
  +-- newo_storage   persistent device settings / NVS
  +-- newo_wifi      station mode, multiple saved APs, reconnect, mDNS
  +-- newo_portal    local setup UI, captive DNS, status APIs
  |
  +-- future: newo_cloud
  +-- future: newo_audio
  +-- future: newo_display
  +-- future: newo_ai
  +-- future: newo_update
```

Telegram is intentionally not a firmware module. Telegram terminates at the VPS and communicates with the ESP32 through `newo_cloud`.

## Phase 1 network flow

```text
boot
  |
  +-- load saved Wi-Fi credentials from NVS
  |
  +-- credentials exist?
       |
       +-- yes --> WiFiMulti tries available saved APs
       |             |
       |             +-- connected --> internet + newo.local + local HTTP API
       |             |
       |             +-- unavailable --> newo@ai.link access point
       |
       +-- no -------------------------> newo@ai.link access point
                                               |
                                               +-- random setup password
                                               +-- captive portal
                                               +-- scan nearby 2.4 GHz networks
                                               +-- save/update/remove credentials
                                               +-- reboot and retry
```

## Why outbound cloud communication

Newo should not require a stable LAN IP and should not expose its ESP32 web server directly to the public Internet. `newo_cloud` will initiate an outbound authenticated TLS connection to a VPS/domain. This lets Newo work behind NAT, DHCP, and networks where clients are isolated from one another.

```text
Newo --> authenticated TLS/WSS :443 --> VPS/domain --> AI/backend/web UI
```

The local `WebServer` remains a setup/status surface, not Newo's public Internet server.

## Telegram path

Telegram belongs on the cloud side of that boundary:

```text
Telegram --HTTPS webhook--> VPS/domain --device channel--> Newo
    ^                            |
    +------ Bot API reply -------+
```

The VPS holds the Telegram bot token and webhook secret, validates Telegram user/chat authorization, and translates bot commands into typed Newo commands. The ESP32 never needs the Telegram bot token.

This prevents Telegram polling or API work from competing with future real-time microphone, speaker, camera, display, or wake-word tasks on the ESP32. It also means the same Newo cloud channel can later serve Telegram, a web UI, AI services, automation, and administration without vendor-specific logic in the firmware.

See [`telegram.md`](telegram.md).

## Storage

Small persistent settings use ESP32 Preferences/NVS. Phase 1 stores a compact JSON list of Wi-Fi credentials under the `newo-wifi` namespace. Larger future assets should use FFat instead of NVS.

The selected `16M Flash (3MB APP/9.9MB FATFS)` Arduino partition scheme contains two 3 MB OTA application slots plus a roughly 9.9 MB FAT filesystem, so it leaves a path for future OTA work while preserving substantial local storage.

## Security boundary

Phase 1 is development firmware. Setup mode creates `newo@ai.link` with a fresh 12-character WPA password generated from the ESP32-S3 hardware RNG and printed to Serial Monitor. The setup page itself is local HTTP inside that WLAN.

Before Newo is treated as a production device:

- setup mode should also be physically gated or explicitly invoked;
- cloud identity must use per-device credentials;
- TLS certificate validation must remain enabled;
- secrets must never be committed to Git;
- credential reset should require a physical action such as a BOOT-button hold;
- Telegram commands must be authorized by user/chat identity on the VPS;
- OTA updates must be authenticated and tested with rollback/recovery behavior.

## Planned modules

### `newo_cloud`

Persistent outbound connection to the VPS/domain, device authentication, heartbeat, remote commands, and AI transport. Telegram uses this same channel instead of talking directly to the ESP32.

### `newo_audio`

I2S microphone input, audio buffering in PSRAM, speaker output through an I2S amplifier, wake-word/VAD integration.

### `newo_display`

Display driver, status UI, setup information, and assistant visual state.

### `newo_ai`

High-level assistant state machine. It should not contain vendor-specific networking; it communicates through `newo_cloud` and local speech/audio modules.

### `newo_update`

OTA firmware update and recovery logic. It is deliberately postponed until Phase 1 networking is verified on hardware.
