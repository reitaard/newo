# Newo2 hardware bring-up

This document records physical bring-up results for the second Newo ESP32-S3 board, referred to here as **Newo2**.

## Board identity

- Board: GOOUUU ESP32-S3-CAM V1.5
- Module: ESP32-S3-WROOM-1 N16R8
- CPU: dual-core ESP32-S3 at 240 MHz
- Flash: 16 MB
- PSRAM: 8 MB
- Camera module physically fitted; ribbon is marked `OV3660`. Camera operation is not yet validated.
- Rear microSD/TF slot present.
- Onboard addressable RGB/WS2812 is mapped to GPIO48 by the board pinout.

Upstream board reference: https://github.com/profharris/GOOUUU_ESP32-S3-CAM

## Physically validated baseline — 2026-09-02

The board was tested alone with only its fitted camera module and microSD card present. No display, microphone, speaker, USB-host device, or other external peripheral was connected.

### Core hardware

| Item | Result |
| --- | --- |
| Chip identification | ESP32-S3 — PASS |
| CPU frequency | 240 MHz — PASS |
| Flash detection | 16 MB — PASS |
| PSRAM detection | 8 MB — PASS |
| Free PSRAM after simple test boot | approximately 7 MB |
| Free heap after simple test boot | approximately 339 KB |

### Onboard RGB

- GPIO48 is reserved for the onboard addressable RGB LED.
- The bring-up sketch issues `rgbLedWrite(48, 0, 0, 0)` at boot and periodically afterwards so the Newo2 hardware test keeps the controllable RGB dark.
- Do not assign GPIO48 to another Newo2 peripheral.

### microSD / SDMMC

The integrated card slot was successfully mounted in 1-bit SDMMC mode using:

```text
GPIO39 -> SD CLK
GPIO38 -> SD CMD
GPIO40 -> SD D0
```

Validated card result:

- Card class reported: SDHC / SDXC
- Reported raw capacity: 59,640 MB (nominal 64 GB card)
- Mounted filesystem size: 59,623 MB
- Read/write test: PASS
- `/newo` directory accessible
- `/newo/sd-test.txt` successfully written and read back

Read-back payload:

```text
Newo SD storage OK
GOOUUU ESP32-S3-CAM V1.5
```

These SD pin assignments are now treated as physically validated for this board and must not be reused by migrated Newo peripherals.

## Test partition note

The minimal hardware test does not use ESP-SR. A normal 16 MB Arduino partition scheme should therefore be used for these standalone tests. The existing Newo `ESP SR 16M` scheme expects `srmodels.bin`; use that scheme again only when the real Newo firmware/ESP-SR stack is migrated to Newo2.

## Camera — next validation

The camera has not yet been initialized in software. Do not mark the camera pin map or sensor operation as validated until the next physical test passes.

The upstream board documentation states that the camera mapping follows the ESP32-S3-EYE style pinout:

```text
SIOD / SDA -> GPIO4
SIOC / SCL -> GPIO5
VSYNC      -> GPIO6
HREF       -> GPIO7
D2 / Y4    -> GPIO8
D1 / Y3    -> GPIO9
D3 / Y5    -> GPIO10
D0 / Y2    -> GPIO11
D4 / Y6    -> GPIO12
PCLK       -> GPIO13
XCLK       -> GPIO15
D7 / Y9    -> GPIO16
D6 / Y8    -> GPIO17
D5 / Y7    -> GPIO18
PWDN       -> not connected
RESET      -> not connected
```

Next acceptance test:

1. Initialize the camera while SD remains mounted.
2. Read and print the detected sensor PID.
3. Capture one JPEG into PSRAM.
4. Save it to `/newo/camera-test.jpg`.
5. Confirm a non-zero JPEG file size and inspect the image on a computer.

Only after that passes should camera pins be considered reserved/validated for the Newo2 migration.

## Migration rule

Newo2 bring-up stays incremental: validate onboard hardware first, then decide the final display, microphone, speaker, and other external pin assignments. Existing Newo pin mappings must not be copied blindly onto Newo2 because the camera and SD slot already consume GPIOs used by the original device.
