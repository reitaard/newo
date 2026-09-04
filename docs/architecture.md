# Newo architecture

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

Only `/face_default` enables autonomous IDLE behavior. All other face commands are persistent manual overrides, while full-screen MESSAGE/ECO content and runtime ERROR, LISTENING, SPEAKING, and THINKING contexts preempt both without erasing the selected manual face.

The display is a procedural character system rather than a set of stored animation frames. The current pipeline is:

```text
energy / curiosity / social / stress + inactivity
        -> behavior episode
        -> autonomous expression intent + 0..100 intensity
        -> semantic presentation cue + pure composition policy
        -> EyePose target + optional effect/caption intent
        -> pose interpolation
        -> gaze motion + resolved motion overlay
        -> Phase-B blink state
        -> resolved secondary effect + caption
        -> renderer
```

The renderer does not know which behavior episode selected the expression. `NewoEyePose` owns compact eye geometry, `NewoEyePoseEngine` owns pose interpolation, `NewoGazeMotion` owns bounded anticipation/travel/settle for meaningful saccades, and `EyeMotionOverlay` resolves transient shake, bounce, stretch, and breathing outside the renderer. The blink service owns bilateral blink/wink scheduling and phase advancement. `newoComposePresentation()` is a pure host-portable policy that converts reusable semantic presentation cues into optional effect/caption intent; runtime supplies the unbiased 0..99 variation roll and owns the timers. Manual `/effect` and `/caption` requests remain separate higher-priority override sources. No animation layer allocates per-frame heap memory or adds a task, full RGB framebuffer, bitmap/GIF animation set, or animation library.

Default autonomous gaze is continuous even when no episode is active. Its current hard envelope is +/-20 px horizontally and +/-12 px vertically; large shifts may use a tiny bounded counter-motion, up to a small bounded overshoot, then settle to the requested destination. Phase B remains the sole owner of normal, double, long, and post-saccade bilateral blinks. Expression openness multiplies blink openness so sleepy eyes reopen as sleepy rather than neutral.

Four volatile `uint8_t` signals—energy, curiosity, social, and stress—update every three seconds. ACTIVE, RELAXED, and DROWSY stages begin at normal idle, two minutes, and five minutes of inactivity. Five coherent episodes are currently available: `CURIOUS_SCAN`, `LOW_ENERGY`, `SOCIAL_ATTENTION`, `ALERT_CHECK`, and `DROWSY_REST`. Base episode spacing is about 8–18 seconds, lengthened in RELAXED and DROWSY states. Episodes emit semantic expression intents such as CURIOUS, HAPPY, TIRED, SLEEPY, SURPRISED, CONFUSED, and SLEEPING; only the pose resolver converts those intents into geometry. Episodes may independently request presentation cues such as CURIOUS, SOCIAL, ALERT_SURPRISE, ALERT_CONFUSED, LOW_ENERGY, and SLEEPING; only the presentation policy converts them into reusable effects/captions.

`DROWSY_REST` is eligible only in the DROWSY stage. It begins TIRED with a mild downward gaze, settles further into SLEEPY, requests the existing long-blink scheduler, rests with proper curved closed lids plus procedural `ZZZ` for a few seconds, wakes through SLEEPY, recenters, and returns to neutral autonomy. Any higher-priority operational context or manual face change resets the episode immediately instead of waiting for it to finish.

Manual `CLOSED`, `DETACHED`, `SLEEPING`, and `UNIMPRESSED` are distinct: CLOSED uses proper curved eyelids, DETACHED preserves the old narrow-slit visual, SLEEPING composes closed eyelids with slow breathing and procedural `ZZZ`, and UNIMPRESSED uses a restrained half-lidded asymmetry with damped horizontal side-eye and no vertical wander. Curved-vs-filled pose transitions use a direction-aware handoff so closing changes to curved geometry late while waking returns to filled geometry early, avoiding a midpoint shape pop.

Presentation vocabulary currently includes procedural `ZZZ`, question/exclamation/surprise marks, ellipsis, sweat, and captions `Huh?`, `Woah!!`, `Hmm...`, `Hey!`, `WTF!!`, and `Tsk!`. Automatic composition is intentionally sparse. `WTF!!` is limited to an extremely strong rare surprise. `UNIMPRESSED`/`Tsk!` are reusable primitives but autonomy currently has no generic disdain trigger, so ordinary idle behavior must not invent annoyance.

USB Serial reports context and episode boundaries immediately. Routine expression/intensity, gaze, blink, presentation, and frame timing are bundled into one compact `[EYES]` summary about every five seconds, with cumulative `EYES_STATS` rate-limited to once per minute. The detailed display-animation contract is documented in [`display-animation.md`](display-animation.md).

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

## Storage and security

Wi-Fi credentials are encoded in a compact JSON list under the `newo-wifi` NVS namespace. They must never be logged or sent through Telegram/cloud telemetry.

Important application events are printed to USB Serial and mirrored into a fixed 64-entry RAM ring buffer. It uses stable level/subsystem/code/detail fields, collapses consecutive identical entries, and is protected for callback/loop access. It is volatile by design: no filesystem or NVS logging, and it disappears on reboot. Health copies only small logger metadata; log export copies at most 40 selected entries into temporary PSRAM, falling back to normal heap and reporting a clean failure if allocation fails. No large ring snapshot is placed on Arduino `loopTask`'s 8 KiB stack. The authenticated WSS channel serves bounded health/log snapshots; the VPS translates codes into readable Telegram text. Public HTTP `/health` remains minimal cloud-service health and never exposes device logs or detailed device telemetry.

BLE Security 1 encrypts provisioning traffic, but null proof-of-possession permits nearby onboarding attempts and is suitable only for this personal prototype. Production work should add per-device PoP/QR material, physical provisioning/reset gating, authenticated OTA with recovery, and hardware regression tests.

Physical BLE provisioning, reboot, and reconnect validation remains pending while the ESP32 is disconnected.
