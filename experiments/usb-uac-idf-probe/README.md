# Newo ESP-IDF USB descriptor probe

This is a temporary, dependency-free ESP-IDF 5.5.x firmware used to get past the
Arduino-ESP32 prebuilt USB Host enumeration buffer limit and inspect the actual
USB microphone descriptors.

It does **not** replace the normal `Newo/` Arduino project. It exists only for
hardware bring-up.

## Why this exists

The normal Newo firmware now reaches USB enumeration but the connected USB audio
device fails before UAC sees it with:

```
ENUM: Configuration descriptor larger than control transfer max length
CHECK_SHORT_CONFIG_DESC FAILED
```

Arduino-ESP32 3.3.10 ships its ESP-IDF libraries prebuilt with a 256-byte USB
control-transfer maximum. This project is built directly by ESP-IDF and sets:

```
CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=2048
CONFIG_USB_HOST_HUBS_SUPPORTED=y
```

It keeps the corrected ESP32-S3 host FIFO allocation used by Newo:

```
RX=72 NPTX=32 PTX=96 TOTAL=200
IN MPS=280, non-periodic OUT MPS=128, periodic OUT MPS=384
```

No endpoint descriptor is rewritten.

## Flash safety

`partitions.csv` is byte-for-byte equivalent in layout to Arduino-ESP32 3.3.10
`esp_sr_16.csv`:

- app0 remains at `0x10000`
- NVS remains at `0x9000`
- the `model`/srmodels partition remains at `0xC10000`

Flashing the probe overwrites the application/bootloader/partition-table images
needed to run the probe, but it does **not** erase NVS or `srmodels.bin` unless you
explicitly run `erase-flash`.

After the test, flashing normal Newo from Arduino restores the regular firmware.
Do not use `erase-flash` for this experiment.

## Requirements

Use ESP-IDF **5.5.4** if possible so the USB stack matches Newo's Arduino-ESP32
3.3.10 baseline. Open an ESP-IDF PowerShell/terminal where `idf.py` is available.

## Build

From this directory:

```powershell
idf.py set-target esp32s3
idf.py build
```

Or from the repository root on Windows:

```powershell
.\tools\build-usb-uac-idf-probe.ps1
```

## Flash and monitor

Replace `COMx` with the serial port used by the original Newo board:

```powershell
idf.py -p COMx flash monitor
```

or:

```powershell
.\tools\build-usb-uac-idf-probe.ps1 -Port COMx
```

For the cleanest test, boot with the OTG hub connected and only one USB audio
microphone attached downstream. A speaker is not needed yet.

## Expected output

The beginning should include:

```
[probe] NEWO USB descriptor probe
[probe] ESP-IDF control-transfer-max=2048 hubs=enabled
[probe] USB host ready FIFO RX=72 NPTX=32 PTX=96 TOTAL=200 ...
[probe] NEW_DEV address=...
```

For each enumerated device the probe prints:

- speed and parent port
- VID/PID and USB/device revision
- manufacturer/product/serial strings when available
- configuration descriptor total length
- every interface and alternate setting
- AudioControl / AudioStreaming classification
- every endpoint address, direction, transfer type, interval and MPS
- UAC1 Type-I channels, bit depth and sample rates when present
- raw class-specific descriptors so unsupported/newer UAC layouts are still visible
- warnings when an endpoint exceeds the current S3 FIFO MPS budget

The external hub itself will also appear as a device. That is expected. The
important block is the downstream microphone/audio device.

This probe does not claim an audio interface and does not stream PCM. Once the
full descriptors are known, the next step is to decide whether the device can be
opened safely with the UAC driver and which alternate setting to use.
