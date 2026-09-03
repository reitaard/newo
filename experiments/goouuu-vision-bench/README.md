# GOOUUU ESP32-S3-CAM vision benchmark

Standalone ESP-IDF firmware used to establish the real camera and face-detection baseline for Newo2 before production integration with Newo.

The experiment still deliberately excludes the production Newo microphone, speaker, display, cloud protocol, SD face database, and face recognition. It isolates capture + local face detection so detector behavior and memory/stability can be measured honestly.

Current working branch: `goouuu-vision-v6-stable`.

## Hardware target

- GOOUUU ESP32-S3-CAM v1.5
- ESP32-S3-WROOM-1 N16R8
- 16 MB flash
- 8 MB Octal PSRAM
- OV3660 camera, physically confirmed PID `0x3660`

Camera pins:

| Signal | GPIO |
| --- | ---: |
| SIOD | 4 |
| SIOC | 5 |
| VSYNC | 6 |
| HREF | 7 |
| XCLK | 15 |
| D7 | 16 |
| D6 | 17 |
| D5 | 18 |
| D4 | 12 |
| D3 | 10 |
| D2 | 8 |
| D1 | 9 |
| D0 | 11 |
| PCLK | 13 |

GPIO38/39/40 are left alone for the physically validated onboard microSD wiring.

## v6 capture architecture

v6 uses the OV3660's **native JPEG VGA** output instead of capturing RGB and re-encoding every browser frame.

```text
OV3660
  -> native JPEG 640x480 q=12
  -> 2 x 128 KiB camera frame buffers in PSRAM
  -> JPEG validation / trim
  -> 3-slot shared JPEG pool
       +-> browser MJPEG stream
       +-> JPEG decode for local detector
```

Camera PSRAM DMA is intentionally OFF because this board/sensor/driver combination was more stable on the non-PSRAM-DMA path.

Malformed frames are checked for JPEG SOI/EOI before being copied into the shared pool. The benchmark records rejects, trims, pool drops, capture timing, stream timing, AI timing, detections, and memory.

## Test modes

### VIEW

Native JPEG camera stream only. No face inference.

### FAST

The current VGA JPEG is decoded directly to a 320x240 RGB565BE buffer and passed to Espressif MSR+MNP. This remains the lower-cost detector path and a useful comparison/scout mode.

### ACCURATE

The current VGA JPEG is decoded at full 640x480 and passed to ESPDet-224. This is the current **primary Newo2 detector candidate** for room-range presence/face acquisition.

### BENCH

Runs the FAST detector path without the browser stream so detector/capture behavior can be measured without MJPEG client overhead.

## RANGE result: rejected

The earlier RANGE experiment split a VGA frame into four overlapping tiles and ran four detector passes. It is intentionally absent from v6.

Physical result from the earlier room test:

- 29.4% detection hit rate (25/85)
- four detector passes
- about 188 ms average detector time plus crop/prep overhead
- at least one non-face object detected in a separate tile

The extra passes did not produce acceptable recall/precision, so tiled RANGE is not a production direction.

## v6 ACCURATE physical result — 2026-09-03

Clean-flashed app version: `6f83166`.

Observed 100-sample ACCURATE window:

| Metric | Result |
| --- | ---: |
| JPEG decode | 52 ms |
| Detect now | 245 ms |
| Detect average | 246.7 ms |
| Detect p95 | 255 ms |
| AI total | 297 ms |
| AI rate | 3.4 fps |
| Detection hit rate | 98.0% (98/100) |
| Face streak | 98 |
| Faces reported | 2 |
| Largest face | 40 x 47 px |
| Free PSRAM | 3.01 MB |
| Largest PSRAM block | 2.81 MB |
| Free internal RAM | 65 KB |
| Sensor PID | 0x3660 |

This is accepted as a strong room-range **presence recall** result. It does not yet prove that every box is a real face. The captured frame reported two boxes, and controlled empty-room/non-face-object testing is still required before selecting the final threshold.

Default detector threshold is `0.30`; the same physical session also exercised `0.40` and `0.20`.

## v6 stability result

The supplied clean-boot log showed:

- normal ESP-IDF 5.5.5 boot;
- correct 16 MB QIO flash;
- correct 8 MB octal PSRAM and successful PSRAM memory test;
- both 128 KiB camera frame buffers allocated;
- OV3660 initialization successful;
- browser stream and both detector models usable;
- no watchdog reset;
- no `FB-OVF`;
- no panic/reboot loop;
- no allocation failure.

Residual camera-driver warnings seen during the session:

```text
cam_hal: NO-EOI - JPEG end marker missing
cam_hal: NO-SOI - JPEG start marker missing
cam_hal: NO-SOI - JPEG start marker missing
```

The system recovered and continued. These warnings remain an explicit soak-test item; they must not be ignored just because v6 did not crash.

## Portable use

The benchmark board creates its own Wi-Fi access point:

- SSID: `NEWO-CAM-TEST`
- password: `newovision`
- page: `http://192.168.4.1/`
- MJPEG: `http://192.168.4.1:81/stream`

This SoftAP is benchmark tooling only. Production Newo2 networking will use the Newo/Newo2 architecture described in `docs/newo2-vision.md`.

## Build

CI is pinned to ESP-IDF 5.5.5.

```bash
cd experiments/goouuu-vision-bench
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.defaults build
idf.py -p COM_PORT flash monitor
```

The v6 custom partition table provides an 8 MB factory app partition. The resulting application is much smaller than that partition, leaving substantial flash headroom for later Newo2 production features.

## Current acceptance test

Before adding face recognition, run ACCURATE for 10-15 minutes with the stream open and record:

- SOI/EOI camera warnings;
- JPEG rejects/trims;
- decode failures;
- pool drops;
- minimum internal RAM;
- minimum/largest PSRAM block;
- watchdog/reset/panic behavior.

Then test an empty room and known non-face objects at thresholds 0.30 and 0.40. The final production threshold should be the lowest value that keeps useful recall without recurring false positives.

## Next phase

Detector development is no longer the main project. The ordered path is now:

1. accept ACCURATE stability/precision with the soak + false-positive tests;
2. add MFN face recognition and measure one recognition operation separately;
3. add identity caching/tracking so MFN is not run every frame;
4. persist a versioned face database on the onboard microSD and keep active embeddings in RAM/PSRAM;
5. implement the small local Newo <-> Newo2 control/event protocol;
6. implement an authenticated VPS Newo2 vision channel for one-shot JPEG requests;
7. feed transcript + requested snapshot to a multimodal model and reuse Newo's existing TTS/speaker path.

See [`../../docs/newo2-vision.md`](../../docs/newo2-vision.md) for the locked Newo2 role and complete roadmap.

Production `Newo/` firmware remains untouched by the benchmark itself.
