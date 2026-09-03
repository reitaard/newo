# Newo architecture

## Two-board production direction

As of 2026-09-03, Newo is intentionally split across two ESP32-S3 boards.

```text
Newo (existing ESP32-S3)
  +-- microphone / I2S input
  +-- WakeNet / voice lifecycle
  +-- authenticated cloud voice path
  +-- speaker / TTS playback
  +-- ST7789 display + user-facing state
  +-- status RGB

Newo2 (GOOUUU ESP32-S3-CAM V1.5)
  +-- OV3660 camera capture
  +-- local face detection
  +-- future local face recognition
  +-- onboard microSD face DB / bounded snapshots
  +-- local vision events to Newo
  +-- direct one-shot snapshot path to VPS for multimodal turns
```

This division is now the project direction. Newo2 is the **camera/vision/storage sidecar**, not a replacement for the original Newo board.

The two boards should exchange only small state/control messages. Raw camera frames, PCM, model buffers, and large files must not be relayed across the inter-board control link. The first production transport candidate is local UDP while both devices are on the same 2.4 GHz LAN, with sequence/request IDs and ACK/retry for important commands. ESP-NOW remains a later router-independent option if needed.

For conversational vision, the existing Newo voice path stays unchanged until the finalized transcript reaches the VPS. If the assistant turn needs visual context, the VPS requests one fresh JPEG directly from Newo2, passes transcript + image to a multimodal model, and sends the answer back through the existing Newo TTS/speaker path. Continuous cloud video is not part of the architecture.

The validated detector baseline is ACCURATE / ESPDet-224 on native-JPEG VGA capture. Physical testing measured about 297 ms total AI latency and 98/100 presence hits in the captured 100-sample window. This is accepted as a strong presence baseline, not proof of zero false positives. The failed four-tile RANGE mode is rejected. Face recognition, detector threshold acceptance, local link, and VPS vision transport remain implementation milestones. See [`newo2-vision.md`](newo2-vision.md) and [`newo2-hardware.md`](newo2-hardware.md).

## Firmware layers

```text
Newo/Newo.ino
  +-- newo_storage   up to eight Wi-Fi credentials in Preferences/NVS
  +-- newo_wifi      scan/rank/connect/recover plus BLE WiFiProv
  +-- newo_cloud     authenticated outbound WSS, telemetry, commands
  +-- newo_audio     local ESP_SR WakeNet plus temporary authenticated WSS PCM sender
  +-- newo_display   ST7789 state, face, and autonomous-eye rendering
  +-- future: AI, update
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

Visible clock preference is a firmware-owned `clock-on` boolean in the existing `newo-wifi` Preferences namespace and defaults ON. Correlated cloud `clock_control` / `clock_ack` messages let Telegram toggle, set, or query it. It only controls clock pixels in the normal face view; `configTzTime`, SNTP, timezone conversion, and internal timekeeping always continue.

## Autonomous eyes

Only the neutral IDLE face receives autonomous behavior. A 20 FPS display-loop state machine chooses a center-biased gaze target, makes a quick bounded saccade, fixes on it, and may make one 1–3 px correction. Its bilateral blink scheduler keeps normal 2.5–6 second intervals, rare double and longer blinks, plus occasional post-saccade blinks; periodic one-eye wink styles remain separate and take priority. Four volatile `uint8_t` values—energy, curiosity, social, and stress—update every three seconds from existing LISTENING, THINKING, SPEAKING, ERROR, and changed-face transitions plus inactivity. They only make small neutral-IDLE adjustments to target weighting, fixation, and blink timing. A separate Phase D owner evaluates an optional micro-behavior every 4–9 seconds: stillness is usual, with infrequent short glances, existing-scheduler blink requests, small lift/squint geometry, a rare wink, or a 500–1200 ms rest-close. ACTIVE, RELAXED, and DROWSY stages begin below two minutes, at two minutes, and at five minutes of inactivity respectively; they only bias these choices. The owner resets on every face/mode transition, so fixed/special faces and operational modes stay isolated. These states do not persist, create tasks, hold audio resources, or require new cloud/Telegram controls. A direct USB-Serial `EYES_STATE` diagnostic is rate-limited to once per minute.

## Cloud boundary

```text
Newo --authenticated certificate-validated WSS :443--> Caddy
      --> 127.0.0.1:8788 Newo Node service
      --> Telegram / later AI services
```

The ESP32 requires no inbound port or stable LAN address. Device authentication uses an ID and bearer credential; server identity uses an explicit trusted CA. Firmware does not fall back to insecure TLS.

The current manual one-turn lifecycle is `OFF → STREAMING → OFF`. `/v` starts one direct microphone session from OFF; a second `/v` cancels it. OFF owns no I2S, ESP_SR, `/voice` socket, sender task, or PCM queue. ARMED and ESP_SR WakeNet remain dormant infrastructure for the future custom Neo wake-word milestone; manual `/v` neither requires nor enables them.

The wake phrase is **model-owned**, not a firmware string. Newo calls `ESP_SR` in wake-word mode but does not select a named WakeNet model in source; the detectable phrase comes from the WakeNet asset packed into the flash `model` partition. The `"MN"` input-format string describes ESP-SR audio channels and is not a phrase. Written identity remains **Newo**, while the spoken name and desired future wake phrase are **Neo**. `server/config/newo-hotwords.txt` contains `NEO` only to bias Sherpa after wake and cannot change WakeNet. See [`wake-word.md`](wake-word.md) for current upstream research and the `Neo` model plan.

A manual `/v` configures I2S directly before starting the one temporary voice task. Arduino's supported `I2S_RX_TRANSFORM_32_TO_16` is the sole INMP441 32-bit-slot-to-PCM16 conversion; streaming selects the configured mono channel from that transformed stereo input. The task opens the authenticated CA-validated `/voice` connection and sends direct 20 ms frames, with no PCM queue. Final transcript, disconnect, send failure, session timeout, or cancellation closes `/voice`, ends I2S, and returns to OFF. A future WakeNet event can still take the same task path and re-arm ARMED; it is not the current trigger.

The packaged Arduino ESP_SR wrapper starts the framework's MultiNet support when its precompiled configuration enables it, even with an empty command list. Newo supplies no commands and never enters command mode; WakeNet is the only feature used. Removing that compiled dependency would require modifying installed framework sources, which Newo intentionally does not do.

The VPS holds Telegram secrets and validates webhook secrets plus user/chat allowlists. Device messages use typed payloads and correlated request IDs. Remote reboot sends `reboot_ack` before the ESP32 schedules restart.

Newo2 will require its own authenticated outbound vision connection before cloud snapshots are enabled. Image bytes should terminate at a dedicated VPS vision handler and must not be tunneled through the existing Newo `/device` control socket. Local presence/identity events can still be mirrored through Newo for UI behavior, but a multimodal request should fetch the JPEG directly from Newo2.

## Storage and security

Wi-Fi credentials are encoded in a compact JSON list under the `newo-wifi` NVS namespace. They must never be logged or sent through Telegram/cloud telemetry.

Important application events are printed to USB Serial and mirrored into a fixed 64-entry RAM ring buffer. It uses stable level/subsystem/code/detail fields, collapses consecutive identical entries, and is protected for callback/loop access. It is volatile by design: no filesystem or NVS logging, and it disappears after reboot. Health copies only small logger metadata; log export copies at most 40 selected entries into temporary PSRAM, falling back to normal heap and reporting a clean failure if allocation fails. No large ring snapshot is placed on Arduino `loopTask`'s 8 KiB stack. The authenticated WSS channel serves bounded health/log snapshots; the VPS translates codes into readable Telegram text. Public HTTP `/health` remains minimal cloud-service health and never exposes device logs or detailed device telemetry.

Newo2's onboard microSD is reserved for the future face database, bounded snapshots, and optional bounded diagnostics. Active face embeddings should be loaded into RAM/PSRAM so recognition does not read SD on every frame. Snapshot/log retention must use explicit quotas/rotation.

BLE Security 1 encrypts provisioning traffic, but null proof-of-possession permits nearby onboarding attempts and is suitable only for this personal prototype. Production work should add per-device PoP/QR material, physical provisioning/reset gating, authenticated OTA with recovery, and hardware regression tests.

Physical BLE provisioning, reboot, and reconnect validation remains pending while the ESP32 is disconnected.
