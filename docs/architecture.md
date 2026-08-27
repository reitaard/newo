# Newo architecture

## Firmware layers

```text
Newo/Newo.ino
  +-- newo_storage   up to eight Wi-Fi credentials in Preferences/NVS
  +-- newo_wifi      scan/rank/connect/recover plus BLE WiFiProv
  +-- newo_cloud     authenticated outbound WSS, telemetry, commands
  +-- future: audio, display, AI, update
```

Telegram is cloud-side and never a firmware dependency.

## Network flow

```text
boot
  -> load saved networks
  -> scan 2.4 GHz and rank visible saved SSIDs by RSSI
  -> connect strongest-first within the 18-second recovery window
       -> connected: authenticated outbound WSS on :443
       -> none reachable: BLE provisioning as PROV_NEWO
            -> Security 1, null PoP prototype
            -> test candidate connection
            -> save/add only on credential success
            -> stop BLE and reboot
```

Existing saved networks are preserved when provisioning adds or updates one entry. Storage is capped at eight. Runtime disconnects trigger bounded scan-first recovery rather than relying on `WiFiMulti` or an arbitrary last-used network.

BLE is provisioning-only. Normal operation is Wi-Fi plus outbound WSS. The firmware contains no SoftAP, captive DNS, local web server, or mDNS service.

## Cloud boundary

```text
Newo --authenticated certificate-validated WSS :443--> Caddy
      --> 127.0.0.1:8788 Newo Node service
      --> Telegram / later AI services
```

The ESP32 requires no inbound port or stable LAN address. Device authentication uses an ID and bearer credential; server identity uses an explicit trusted CA. Firmware does not fall back to insecure TLS.

The VPS holds Telegram secrets and validates webhook secrets plus user/chat allowlists. Device messages use typed payloads and correlated request IDs. Remote reboot sends `reboot_ack` before the ESP32 schedules restart.

## Storage and security

Wi-Fi credentials are encoded in a compact JSON list under the `newo-wifi` NVS namespace. They must never be logged or sent through Telegram/cloud telemetry.

BLE Security 1 encrypts provisioning traffic, but null proof-of-possession permits nearby onboarding attempts and is suitable only for this personal prototype. Production work should add per-device PoP/QR material, physical provisioning/reset gating, authenticated OTA with recovery, and hardware regression tests.

Physical BLE provisioning, reboot, and reconnect validation remains pending while the ESP32 is disconnected.
