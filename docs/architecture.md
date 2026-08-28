# Newo architecture

## Firmware layers

```text
Newo/Newo.ino
  +-- newo_storage   up to eight Wi-Fi credentials in Preferences/NVS
  +-- newo_wifi      scan/rank/connect/recover plus BLE WiFiProv
  +-- newo_cloud     authenticated outbound WSS, telemetry, commands
  +-- newo_audio     I2S/DMA microphone capture plus authenticated WSS PCM sender
  +-- future: display, AI, update
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

The microphone test opens a separate authenticated `/voice` WSS connection using those same headers and CA. `newo_audio` reads INMP441 24-bit samples from 32-bit I2S stereo slots in a dedicated FreeRTOS task, explicitly converts the selected L/R slot to 16 kHz signed PCM16, and enqueues 20 ms frames. A separate dedicated voice FreeRTOS task exclusively owns the `/voice` WebSocket and joins five frames into a 100 ms raw PCM message; the Arduino loop continues servicing Wi-Fi and `/device` independently. It drops stale queued frames on a disconnect/reconnect rather than replaying delayed speech.

A physical test found synchronous `sendBIN()` stalls of 1.49–5 seconds. ArduinoWebsockets 2.7.2 implements `sendBIN()` as a synchronous write loop with a five-second `WEBSOCKETS_TCP_TIMEOUT`, so a task cannot cancel a write before the underlying TLS/TCP call returns. Moving the socket to the voice task isolates that library limitation from the control loop, but does not make the library's write non-blocking. The 24-frame queue saturated, capture overran, and ASR degraded to fragments; a later fresh `/voice` connection, after stale audio was discarded, restored accurate recognition. `newo_audio` therefore tracks a voice-only HEALTHY → DEGRADED → RESETTING → COOLDOWN state machine. Sustained queue saturation, repeated/very slow sends, or overrun growth at high queue depth reset only `/voice`: sending stops, the socket closes, queue/bundle state is cleared, and a fresh authenticated `/voice` connection begins. Wi-Fi, `/device`, Telegram, I2S, and the ESP are not restarted. A 10-second cooldown prevents reset loops. The authenticated `/device` `voice_reset` request invokes that exact reset routine.

The VPS holds Telegram secrets and validates webhook secrets plus user/chat allowlists. Device messages use typed payloads and correlated request IDs. Remote reboot sends `reboot_ack` before the ESP32 schedules restart.

## Storage and security

Wi-Fi credentials are encoded in a compact JSON list under the `newo-wifi` NVS namespace. They must never be logged or sent through Telegram/cloud telemetry.

Important application events are printed to USB Serial and mirrored into a fixed 64-entry RAM ring buffer. It uses stable level/subsystem/code/detail fields, collapses consecutive identical entries, and is protected for callback/loop access. It is volatile by design: no filesystem or NVS logging, and it disappears after reboot. Health copies only small logger metadata; log export copies at most 40 selected entries into temporary PSRAM, falling back to normal heap and reporting a clean failure if allocation fails. No large ring snapshot is placed on Arduino `loopTask`'s 8 KiB stack. The authenticated WSS channel serves bounded health/log snapshots; the VPS translates codes into readable Telegram text. Public HTTP `/health` remains minimal cloud-service health and never exposes device logs or detailed device telemetry.

BLE Security 1 encrypts provisioning traffic, but null proof-of-possession permits nearby onboarding attempts and is suitable only for this personal prototype. Production work should add per-device PoP/QR material, physical provisioning/reset gating, authenticated OTA with recovery, and hardware regression tests.

Physical BLE provisioning, reboot, and reconnect validation remains pending while the ESP32 is disconnected.
