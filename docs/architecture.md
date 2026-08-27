# Newo architecture

## Goal

Newo is a portable ESP32-S3 assistant endpoint. The ESP32 handles device I/O, local provisioning, connectivity, and real-time peripheral work; larger AI inference can live on a VPS, home GPU, or other backend.

The firmware is split into small modules so later audio, display, AI, cloud, and OTA work does not turn the main sketch into one large file.

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
       |             +-- unavailable --> Newo-Setup access point
       |
       +-- no -------------------------> Newo-Setup access point
                                               |
                                               +-- captive portal
                                               +-- scan nearby 2.4 GHz networks
                                               +-- save/update/remove credentials
                                               +-- reboot and retry
```

## Why outbound cloud communication

Newo should not require a stable LAN IP and should not expose its ESP32 web server directly to the public Internet. Later, `newo_cloud` will initiate an outbound authenticated TLS connection to a VPS/domain. This lets Newo work behind NAT, DHCP, and networks where clients are isolated from one another.

```text
Newo --> HTTPS/WSS :443 --> VPS/domain --> AI/backend/web UI
```

The local `WebServer` remains a setup/status surface, not Newo's public Internet server.

## Storage

Small persistent settings use ESP32 Preferences/NVS. Phase 1 stores a compact JSON list of Wi-Fi credentials under the `newo-wifi` namespace. Larger future assets should use FFat instead of NVS.

The selected `16M Flash (3MB APP/9.9MB FATFS)` Arduino partition scheme contains two 3 MB OTA application slots plus a roughly 9.9 MB FAT filesystem, so it leaves a path for future OTA work while preserving substantial local storage.

## Security boundary

Phase 1 is development firmware. The setup AP is intentionally open for quick bring-up. Before Newo is treated as a production device:

- setup mode must be authenticated or physically gated;
- cloud identity must use per-device credentials;
- TLS certificate validation must remain enabled;
- secrets must never be committed to Git;
- credential reset should require a physical action such as a BOOT-button hold;
- OTA updates must be authenticated and tested with rollback/recovery behavior.

## Planned modules

### `newo_cloud`

Persistent outbound connection to the VPS/domain, device authentication, heartbeat, remote commands, and AI transport.

### `newo_audio`

I2S microphone input, audio buffering in PSRAM, speaker output through an I2S amplifier, wake-word/VAD integration.

### `newo_display`

Display driver, status UI, setup information, and assistant visual state.

### `newo_ai`

High-level assistant state machine. It should not contain vendor-specific networking; it communicates through `newo_cloud` and local speech/audio modules.

### `newo_update`

OTA firmware update and recovery logic. It is deliberately postponed until Phase 1 networking is verified on hardware.
