# Newo2 production firmware

`Newo2/` is the production ESP-IDF project for the GOOUUU ESP32-S3-CAM V1.5. Historical camera/face work remains under `experiments/` and on the `goouuu-*` branches. CSI remains under `experiments/wifi-sensing-v1/` and is not copied into this firmware.

## Production v1 scope

Only three camera capabilities are enabled:

1. logical camera ON/OFF;
2. cheap local motion detection;
3. one-shot still capture on a trigger.

No MJPEG stream, face detector, face recognition, VLM, OCR, CSI inference, or video recorder is part of v1.

### Why this differs from the old v6 benchmark

The old v6 firmware was correct for its job: continuously publish JPEGs to a browser and continuously benchmark MSR/MNP or ESPDet. On this exact board the ACCURATE path measured about 52 ms JPEG decode + 247 ms detection (~297 ms total, ~3.4 fps). That cost is unnecessary merely to answer "did the scene change?".

Production v1 therefore keeps what the physical tests proved — OV3660 pin map, native JPEG, quality 12, PSRAM, `CONFIG_CAMERA_PSRAM_DMA=n`, JPEG SOI/EOI validation and GPIO48 dark — while removing the benchmark stream, detector models and three-slot fan-out pool. It uses one framebuffer with `CAMERA_GRAB_WHEN_EMPTY`, matching the deterministic one-shot pattern used by newer ESP32-S3 camera projects such as MomentScraper.

Motion is a generic trigger, not a camera action. It decodes QVGA JPEG to 160x120 luminance, samples the frame, applies warm-up/confirmation/hysteresis, and emits `MOTION_DETECTED`. The `skills/motion_snapshot/` consumer then switches temporarily to the physically validated SVGA 800x600 quality-12 profile, captures one JPEG, restores the motion profile, saves to SD when available, and uploads directly to the VPS.

This layering means later triggers can request the same skill without changing camera code:

```text
motion ---------┐
Telegram -------┼--> event bus --> skills/motion_snapshot --> camera --> SD + VPS
CSI ------------┤
Newo voice -----┤
automation -----┘
```

Future video work belongs in another folder such as `skills/video/`; it must not grow inside `motion_snapshot`.

## Boot behavior

The OV3660 is initialized and kept warm, but **logical camera state boots OFF**. Motion is configured ON but cannot acquire frames until the camera is enabled. This avoids camera reinitialization latency while preserving an explicit privacy state. A later separately-tested hard/privacy power-down mode can be added without changing trigger APIs.

## Hardware choices retained from physical validation

- GOOUUU ESP32-S3-CAM V1.5 / ESP32-S3-WROOM-1 N16R8
- OV3660 PID 0x3660
- 16 MB flash / 8 MB octal PSRAM
- camera XCLK 20 MHz
- JPEG quality 12
- PSRAM DMA OFF
- GPIO48 WS2812 forced black/off
- SDMMC 1-bit: CLK 39, CMD 38, D0 40
- still profile: SVGA 800x600 (already physically validated on this board)

UXGA is intentionally not the first production still profile. MomentScraper demonstrates fast SVGA->UXGA switching on ESP32-S3, but Newo2's own physical record already proves SVGA; first production validation should change one variable at a time.

## Local secrets

Copy:

```bash
cp Newo2/main/newo2_secrets.example.h Newo2/main/newo2_secrets.h
```

Fill Wi-Fi, a unique Newo2 device secret, and the VPS host. Never reuse Newo's device secret. `newo2_secrets.h` is ignored by Git.

## Build and flash

```bash
cd Newo2
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.defaults build
idf.py -p COM5 erase-flash
idf.py -p COM5 flash monitor
```

Use the actual COM port shown by Windows; COM5 is only the previously validated port. `erase-flash` intentionally removes the current experimental firmware and its NVS.

Expected first boot includes `PID=0x3660`, SD mount (if card is fitted), `logical camera OFF`, Wi-Fi/cloud connection, and `ready: camera=OFF motion=ON`.

## VPS bridge

`server/src/newo2-bridge.js` is intentionally a separate process so Newo2 can evolve without destabilizing the mature Newo voice socket. It binds localhost by default on port 8792. It provides:

- authenticated WSS `/newo2/device` for small commands/events;
- authenticated HTTPS `/newo2/snapshot` for JPEG upload;
- localhost-only admin controls for camera, photo, recording, streaming, settings and status;
- bounded VPS snapshot retention;
- optional Telegram `sendPhoto` using the existing bot token.

Start from the `server/` directory after adding `.env.newo2.example` values to `.env`:

```bash
npm run start:newo2
```

For PM2, run the same entry point as a second app. Caddy should proxy **only** the two device-facing paths to `127.0.0.1:8792`; do not expose `/newo2/admin/*`:

```caddyfile
@newo2 path /newo2/device /newo2/snapshot
reverse_proxy @newo2 127.0.0.1:8792
```

Local VPS controls require the separate `NEWO2_ADMIN_SECRET` bearer token.
Later Telegram commands simply map onto these:

```bash
curl -s -H "Authorization: Bearer $NEWO2_ADMIN_SECRET" http://127.0.0.1:8792/newo2/admin/status
curl -s -X POST -H "Authorization: Bearer $NEWO2_ADMIN_SECRET" -H 'content-type: application/json' -d '{"enabled":true}' http://127.0.0.1:8792/newo2/admin/camera
curl -s -X POST -H "Authorization: Bearer $NEWO2_ADMIN_SECRET" http://127.0.0.1:8792/newo2/admin/snapshot
```

The SD card uses a 500-slot snapshot ring (`snapshot-000.jpg` through
`snapshot-499.jpg`). Videos are never rotated or deleted automatically; an SD
write failure stops recording and preserves the partial MJPEG file.

## First physical acceptance test

Phase 1 intentionally contains no sensing or automatic detection. Hardware
acceptance covers clean boot with logical camera OFF, manual photo, recording,
private live stream, settings persistence, SD files, reconnect and power loss.
