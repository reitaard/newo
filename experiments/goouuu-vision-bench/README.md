# GOOUUU ESP32-S3-CAM vision benchmark

Standalone firmware for measuring the real camera and face-detection limits of the GOOUUU ESP32-S3-CAM before changing the production Newo wiring.

This first phase deliberately does **not** use the Newo cloud, speaker, microphone, display, SD card, USB host, or face recognition. It isolates the camera + detector so the measurements are useful.

## Hardware target

- GOOUUU ESP32-S3-CAM v1.5
- ESP32-S3-WROOM-1 N16R8
- 16 MB flash
- 8 MB Octal PSRAM
- OV3660 camera on the board currently being tested

Camera pins used by the firmware:

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

The firmware leaves GPIO38/39/40 alone, so the onboard SD wiring remains available for the later persistence phase.

## Test modes

### VIEW

Native JPEG stream only. Use this for field-of-view, focus, lighting, camera orientation, and placement checks. No face inference is run.

### DETECT

The same JPEG frame is decoded to RGB888 in PSRAM, passed through Espressif `HumanFaceDetect` (default MSR + MNP model), and then streamed to the browser. Bounding boxes are drawn by the browser as an overlay rather than re-encoding the image after drawing.

Reported values include:

- current detection time
- rolling average detection time
- p95 detection time
- JPEG decode time
- capture time
- total capture + decode + detection pipeline time
- last-100 detection hit rate
- consecutive face-detection streak
- detected face count
- largest face width/height in source pixels
- free heap and PSRAM

### BENCH

Stops the browser video intentionally and loops capture + JPEG decode + face detection as quickly as possible. This separates detector performance from MJPEG/Wi-Fi/browser overhead.

## Resolutions

The web UI can switch between:

- QVGA 320x240
- VGA 640x480
- SVGA 800x600

This is specifically for the room-range experiment. Record both distance and the reported face bounding-box size; the pixel size is the more portable measurement.

## Portable use

The board creates its own Wi-Fi access point. A router or VPS is not required.

- SSID: `NEWO-CAM-TEST`
- password: `newovision`
- page: `http://192.168.4.1/`

Power the GOOUUU from a USB power bank, connect a phone to the AP, open the page, and physically move the camera around the room.

## Build

Use ESP-IDF 5.5.x. CI is pinned to ESP-IDF 5.5.5.

```bash
cd experiments/goouuu-vision-bench
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS=sdkconfig.defaults build
idf.py -p COM_PORT flash monitor
```

On Windows, replace `COM_PORT` with the GOOUUU TTL USB-C serial port (for example `COM6`).

The native OTG USB-C port is not needed for this benchmark.

## What to record during the range test

For each candidate camera position, test at several distances and head angles. A useful sheet is:

| Distance | Angle | Resolution | Face px | Hits / 100 | Detect avg | Detect p95 | Lighting |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 m | front | QVGA | | | | | |
| 2 m | front | QVGA | | | | | |
| 3 m | front | QVGA | | | | | |
| 4 m | front | QVGA | | | | | |

Repeat promising points at VGA/SVGA and at roughly 15°, 30°, and 45° head angles.

## Next phase

Only after this detector/range benchmark is stable:

1. add MFN face recognition and measure recognition latency separately;
2. add enroll/list/forget operations;
3. mount the face database on the onboard SD card and load active embeddings into PSRAM;
4. test identity caching/tracking;
5. design the local link to the existing Newo ESP32-S3.

Production `Newo/` firmware is intentionally untouched on this branch.
