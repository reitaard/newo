# Newo2 hardware bring-up

This document records physical bring-up results for the second Newo ESP32-S3 board, referred to here as **Newo2**.

## Board identity

- Board: GOOUUU ESP32-S3-CAM V1.5
- Module: ESP32-S3-WROOM-1 N16R8
- CPU: dual-core ESP32-S3 at 240 MHz
- Flash: 16 MB
- PSRAM: 8 MB
- Camera module physically fitted and now physically validated as **OV3660**.
- Rear microSD/TF slot present and physically validated.
- Onboard addressable RGB/WS2812 is GPIO48 and is deliberately kept dark during bring-up.
- Onboard buttons physically present on this board are RST and BOOT; BOOT is GPIO0.

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
- The bring-up sketches issue `rgbLedWrite(48, 0, 0, 0)` at boot and periodically afterwards so the Newo2 hardware test keeps the controllable RGB dark.
- Do not assign GPIO48 to another Newo2 peripheral.

### Onboard buttons

The physical board has two onboard buttons: **RST** and **BOOT**.

- RST has been repeatedly used successfully during bring-up and resets the board as expected.
- BOOT is GPIO0 and was tested with `INPUT_PULLUP` after normal boot.
- Observed sequence:

```text
[boot] initial=RELEASED
[boot] PRESSED
[boot] RELEASED
```

Result: **PASS**.

GPIO0 is a strapping/boot pin, so Newo2 may use the BOOT button for an intentional user action after startup, but GPIO0 must not be treated as an unrestricted peripheral pin and the button must not be held during reset unless download/boot behavior is intended.

The reference board documentation also labels GPIO3 as `SW` and GPIO46 as `SH`/shutter header functions. Those are not additional physical buttons on the tested V1.5 board, so they are not marked as onboard-button hardware.

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

These SD pin assignments are physically validated for this board and must not be reused by migrated Newo peripherals.

## Camera — physically validated

The fitted camera was successfully initialized twice while the microSD card remained mounted. The sensor identified itself as:

```text
PID = 0x3660
sensor = OV3660
VER = 0x00
MIDH = 0x00
MIDL = 0x00
```

The working camera pin map is therefore treated as physically validated for Newo2:

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

Initial camera validation settings:

- XCLK: 20 MHz
- Pixel format: JPEG
- Frame size: SVGA, 800 x 600
- JPEG quality setting: 12
- Frame buffers: 1
- Frame buffer location: PSRAM
- Grab mode: `CAMERA_GRAB_WHEN_EMPTY`

Two consecutive physical capture runs passed:

| Run | Warm-up JPEG | Saved JPEG | Resolution | SD verification |
| --- | ---: | ---: | --- | --- |
| 1 | 14,971 bytes | 14,708 bytes | 800 x 600 | exact size match — PASS |
| 2 | 14,012 bytes | 13,723 bytes | 800 x 600 | exact size match — PASS |

The output file was written to:

```text
/newo/camera-test.jpg
```

Camera-to-storage path now physically validated:

```text
OV3660 -> ESP32-S3 camera interface -> PSRAM framebuffer -> JPEG -> microSD
```

After camera initialization, reported free PSRAM was approximately 8,063 KB. The running heartbeat after capture reported approximately 303 KB free heap and 8,063 KB free PSRAM.

### Visual JPEG validation

Because no separate SD-card reader was available, a temporary Newo2 SoftAP/web-server sketch mounted the same microSD card and served `/newo/camera-test.jpg` over local Wi-Fi. The image was successfully opened and visibly rendered in a browser.

This adds a visual validation layer beyond the exact file-size check:

```text
OV3660 capture -> JPEG -> microSD -> Newo2 HTTP server -> browser image
```

Result: **PASS**.

The temporary viewer used local AP `NEWO2-CAM`; it is bring-up tooling only and is not part of the production Newo2 network design.

The camera pin assignments above are now reserved for Newo2 and must not be reused for the migrated microphone, display, speaker, or other external peripherals.

## Native USB / OTG — device path partially validated

With the existing TTL USB-C connection left attached for programming/serial, the board's separate USB-C port marked **OTG** was connected to the same Windows PC with a second data cable.

Observed result:

- Windows produced the USB device-connect sound.
- Arduino IDE showed a new **COM5** entry when the OTG cable was attached.
- The TTL/programming path remained separately available.
- No SD-card drive appeared in Windows at this stage, which is expected because the running button-test firmware did not expose a USB Mass Storage Class device.

This confirms that the physical OTG connector and native ESP32-S3 USB data path enumerate successfully as a USB device. Result: **PASS for native USB enumeration**.

USB Mass Storage exposure of the onboard microSD is still pending and is the next bring-up test.

## Combined onboard result

As of 2026-09-02, the following built-in Newo2 hardware is physically proven:

```text
ESP32-S3            PASS
16 MB flash         PASS
8 MB PSRAM          PASS
GPIO48 RGB off      PASS
RST button          PASS
BOOT / GPIO0        PASS
64 GB microSD       PASS
OV3660 camera       PASS
Camera -> SD JPEG   PASS
Saved JPEG visually PASS
Native USB enumerate PASS
USB SD mass storage PENDING
```

A small amount of garbled serial text was observed around one reset / sketch transition, but the subsequent boot, SD mount, camera initialization, capture, file write, exact-size verification, heartbeat, browser image retrieval, button test, and native USB enumeration all completed normally. It is not treated as a hardware failure.

## Vision baseline — 2026-09-03

The clean-flashed ESP-IDF `goouuu-vision-v6-stable` build physically validated Newo2 as a usable local vision board rather than only a camera/storage board.

Validated runtime configuration:

```text
App version       6f83166
ESP-IDF           5.5.5
Camera            OV3660 / PID 0x3660
Capture           native JPEG VGA 640x480 q=12
Frame buffers     2 x 128 KiB PSRAM
PSRAM DMA         OFF
Primary detector  ESPDet-224 / ACCURATE
```

Observed ACCURATE 100-sample window:

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
| Faces in captured frame | 2 |
| Largest face | 40 x 47 px |
| Free PSRAM | 3.01 MB |
| Largest PSRAM block | 2.81 MB |
| Free internal RAM | 65 KB |

The earlier four-tile RANGE mode is rejected: its comparable physical test produced only 29.4% hit rate and also produced a non-face detection in a separate tile. RANGE is not part of the Newo2 production plan.

The v6 serial log contained no watchdog reset, framebuffer overflow, panic, reboot loop, or allocation failure. It did contain one `NO-EOI` and two `NO-SOI` camera/JPEG framing warnings; the driver recovered and the firmware continued. Those warnings remain a soak-test item before the production camera service is declared final.

The ACCURATE screenshot reported two boxes, but the screenshot alone cannot prove both boxes are true faces. The 98% value is therefore accepted as strong **presence recall**, not proof of false-positive-free multi-face precision. Threshold and precision still require a controlled empty-room/non-face-object test.

Newo2's role is now locked as the **camera/vision/storage module for Newo**. The production firmware, face recognizer, Newo-to-Newo2 protocol, and VPS multimodal snapshot channel are still pending. See [`newo2-vision.md`](newo2-vision.md) for the current architecture decision and ordered roadmap.

## Test partition note

The standalone Newo2 hardware tests do not use ESP-SR. A normal 16 MB Arduino partition scheme should therefore be used for these bring-up sketches. The existing Newo `ESP SR 16M` scheme expects `srmodels.bin`; switch back to that scheme only when the real Newo firmware/ESP-SR stack is migrated to Newo2.

The current ESP-IDF vision benchmark instead uses its own 16 MB custom partition table with an 8 MB factory app partition. Newo2 does not need ESP-SR because audio/wake-word responsibilities remain on the original Newo board.

## Migration rule

Newo2 bring-up stays incremental. Camera and SD pins are now reserved and must not be reused by migrated peripherals. The current architecture no longer plans to migrate the original microphone/display/speaker stack onto Newo2: Newo2 is the dedicated camera/vision/storage sidecar, while the existing Newo board remains the audio/display/assistant interaction device.
