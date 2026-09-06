# Experimental native USB audio

This adds USB Audio Class diagnostics and bounded standalone tests alongside the
existing MSC host. I2S microphone, I2S speaker, voice lifecycle, cloud and `/usb`
are unchanged. Audio never starts on insertion. One physical audio device can be
tested at a time; a hub/flash drive can share the installed host library.

## Framework and source evidence

Validated baseline: `main` `5f34ccf4ca4b3474d199bd300daf977a8b9ab709`,
Arduino-ESP32 **3.3.10**, packaged ESP-IDF **5.5.4**, ESP32-S3, QIO 16MB,
OPI PSRAM, `esp_sr_16`. No framework upgrade or managed USB-library replacement.

The [official UAC 1.5.0 component](https://components.espressif.com/components/espressif/usb_host_uac/versions/1.5.0/readme)
uses `usb_host_install()` followed by `uac_host_install()`. It supports **UAC 1.0**,
PCM capture and playback, with independent logical interfaces for RX/TX on one
physical device. It does not install another host daemon. Its manifest requires
IDF >=5.0; the managed `usb` dependency applies to IDF >=6.0. Arduino uses the
already packaged IDF 5.5.4 USB APIs, FreeRTOS and esp_ringbuf.

Sources are pinned to Espressif's published 1.5.0 repository revision
[`6d24137e14a4f6c8138662a7a074be16a899c2c6`](https://github.com/espressif/esp-usb/tree/6d24137e14a4f6c8138662a7a074be16a899c2c6/host/class/uac/usb_host_uac).
`Newo/src/usb_host_uac/` contains the two C translation units, headers, license,
manifest and Arduino configuration shim. Arduino compiles `src/` recursively;
it does not execute component CMake/Kconfig. `cmake_utilities` is build metadata
and is not needed by these translation units. Local changes are listed in that
directory's README and patch. No audio transport was implemented from scratch.

## Why MPS 208 failed against 128

The installed `qio_opi/include/sdkconfig.h` defines
`CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT=1` and enables external/multilevel hubs.
In [IDF 5.5.4 hcd_dwc.c](https://github.com/espressif/esp-idf/blob/v5.5.4/components/usb/hcd_dwc.c),
`_calculate_fifo_from_bias()` assigns 34 RX lines and 16 nonperiodic TX lines
on S3; the remaining 206 lines serve periodic OUT.
In [usb_dwc_hal.c](https://github.com/espressif/esp-idf/blob/v5.5.4/components/hal/usb_dwc_hal.c),
`usb_dwc_hal_get_mps_limits()` computes:

| Direction | Formula | Previous default | New allocation |
|---|---|---:|---:|
| IN | `(RX lines - 2) * 4` | 128 bytes | 504 bytes |
| Nonperiodic OUT (control/bulk) | `NPTX lines * 4` | 64 bytes | 128 bytes |
| Periodic OUT (isochronous/interrupt) | `PTX lines * 4` | 824 bytes | 384 bytes |

The new allocation is RX=128, NPTX=32, PTX=96: exactly the S3's 256 four-byte
FIFO lines. It is supplied through the installed public
[`usb_host_config_t::fifo_settings_custom`](https://github.com/espressif/esp-idf/blob/v5.5.4/components/usb/include/usb/usb_host.h)
before the **single** host installation. This preserves full-speed MSC bulk
packet capacity. It trades unused large periodic OUT capacity for capture;
devices needing OUT MPS >384 or IN MPS >504 remain unsupported by this allocation.

HCD checks the descriptor's **maximum** packet size at interface claim time.
Reducing the sample rate does not reduce `wMaxPacketSize` on the same alternate.
A different alternate with smaller MPS can help; rewriting 208 to 128 cannot.
Espressif UAC ultimately calls the same `usb_host_interface_claim()` and therefore
cannot bypass FIFO capacity. Its alternate selection uses channels, bit depth
and sample rate; the local fix uses the actual advertised alternate number
instead of assuming array index + 1.

For `001f:0b21` / `GHW-136D1-20230927` / `USB Audio`, the previous log alone does
**not** identify the endpoint direction, format or claimed alternate. Under the
verified current default, a 208-versus-128 rejection is consistent with an IN
endpoint. 208 is an advertised capacity, not proof of 48kHz/16-bit/stereo (which
normally averages 192 bytes/ms). Device clock tolerance, another format or a
vendor capacity choice cannot be distinguished without descriptors. Capture the
new log to establish which alternate advertises 208 and its rates. The baseline
monitor claims **no** interfaces; its MSC driver filters MSC class/subclass/BOT
before claiming. The old failure cannot be attributed to that monitor choosing
an audio alternate based solely on the supplied log.

## Diagnostics and supported test subset

Insertion prints VID/PID, cached USB manufacturer/product strings (non-ASCII
characters replaced with `?`), speed, AudioControl version, audio interfaces,
alternates and every endpoint including feedback. Type-I UAC1 records include
format tag, channels, subframe size, bits and discrete rates/continuous range.
UAC2/3 protocols remain diagnostics-only; rates needing clock-control queries
are explicitly unavailable, not interpreted as UAC1. MIDI-only devices are
classified as Audio class but have no capture/playback test format.

The test policy supports full-speed, 1ms interval, mono/stereo signed PCM16 in
two-byte subframes. Preferred MIC rate is 16kHz; preferred SPK rate is 48kHz;
44.1/32/24/16/8kHz alternatives are considered as advertised. Firmware checks
packet capacity and the alternate that the driver's matching algorithm selects.
It refuses explicit/implicit-feedback layouts and asynchronous OUT requiring
feedback: this driver does not implement the feedback endpoint. It also refuses
non-1ms intervals rather than applying upstream's descriptor rewrite. These are
test/driver restrictions, not declarations that the hardware device is broken.
Descriptors are bounded to 16 streaming alternates and 16 rates per alternate;
truncation or malformed configuration disables tests.

## Tasks, memory and hot plug

The host, MSC and VFS workers remain. The existing monitor becomes the audio
application worker (8192-byte stack instead of 3072); one UAC event task adds a
4096-byte stack at priority 2. `loop()` does not parse commands or service audio.
Callbacks only mark disconnect/error flags; the monitor worker owns API calls
and final close. Close retains ownership and retries while transfers are pending.
The monitor retains a reference to the selected audio device through unplug.

Each open stream has an 8192-byte driver ring; duplex therefore has 16KiB of
bounded PCM buffering. One shared 1920-byte scratch array generates/analyzes PCM.
Three URBs of three packets use **9 × endpoint MPS** bytes per stream in
USB-allocated DMA-capable internal memory, plus transfer descriptors/driver
metadata. At the allowed MPS maxima that is 4536 RX + 3456 TX bytes of payload.
No explicit PSRAM audio allocation is added; driver rings use `xRingbufferCreate`
and follow the framework allocator. Large production audio buffers remain as
before. Dynamic heap/PSRAM and monitor stack headroom are reported on hardware;
the compiler's static RAM summary does not include tasks, rings or URBs.

MIC reports application bytes/s, effective per-channel sample rate, cumulative
successful packets, packet errors (including late/missed), dropped packets,
buffer high-water, RMS/peak normalized to full scale, heap/internal heap/PSRAM and
monitor stack headroom. These are observed USB completion counters, not inferred
packet counts. SPK bytes/s is **queued** PCM throughput; packet counters identify
actual USB completions. TX reports submission errors, transfer errors, ring write
retries, and starvation. Total underruns include the intentional finite waveform
tail; starvation seen while still generating is reported separately. No PCM is
printed, saved to flash or routed into Newo voice/TTS.

## Physical test procedure

Use the existing powered USB OTG wiring on GPIO19/20, and open the hardware CDC
serial port (currently COM5) at 115200 baud with newline line endings. Close other
serial monitors before upload. Keep headphone volume low for the first tone.

1. Boot without an audio device. Confirm `[usb] HOST_READY`, the FIFO allocation
   above, and `[usb-uac] ... driver=ready`. Check display, Wi-Fi/cloud, I2S voice
   and I2S speaker behavior.
2. Insert a known FAT/FAT32 drive directly: confirm MSC connection and
   `[usb] MOUNTED ... /usb`. Remove it: confirm unmount. Repeat through the hub.
3. Insert the DAC. Save the complete `[usb-uac]` descriptor output, especially
   VID/PID 001f:0b21, every MPS 208 endpoint and its direction/interface/alternate.
   Confirm there is no attempt to mount the DAC as MSC. Insertion is silent.
4. Send `uac mic`. Speak/tap near its microphone. For five seconds RMS/peak must
   respond; bytes/s should approach `rate × channels × 2`, with no packet errors,
   drops or transfer errors. Zero-valued PCM proves transport only, not a working
   analog microphone. The test closes automatically.
5. Send `uac tone`. Listen for a one-second 1kHz tone at roughly -30dBFS with short
   ramps, followed by silence. It closes after 1.5 seconds. Confirm USB completion
   packets and no generation-time starvation/transfer errors. A transport pass
   cannot establish analog audibility or unmuted hardware.
6. After both individual transport passes, send `uac duplex`. This runs independent
   capture plus the same finite tone, never mic-to-speaker feedback. It ends within
   about three seconds. Inspect both counters and listen/watch for service stalls;
   a short test is not a long-duration stability certification.
7. Send `uac status` for selection/test state or `uac stop` for early cleanup.
   Unplug during capture, tone, duplex and an interface start. Require
   `disconnected` then `cleanup complete`, no panic/watchdog, and successful
   reconnect/retest. After reconnect, repeat individual tests before duplex.
8. Repeat with hub + DAC + mounted flash drive. Check `/usb` mount/unmount during
   tests and that Newo's normal display/network/I2S services remain responsive.
   Track internal heap/PSRAM across at least ten unplug/replug cycles for leaks.

USB bandwidth, eight hardware host channels (including hub/control/bulk/feedback
needs), hub power and scheduling can limit simultaneous devices. Compilation
cannot prove MSC runtime coexistence, analog sound, capture levels or hotplug
stability. Record actual hardware results separately from build/host-test results.

## Reproduce validation

Run `tools/run-usb-audio-host-test.ps1` (g++/MinGW) for descriptor bounds and format
policy tests, including the 208-byte case and non-contiguous alternates. Build:

```powershell
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=esp_sr_16,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default' --build-path build/usb-uac-3.3.10 Newo
```

Only upload when authorized. The user's subsequent COM5 request authorizes the
upload in this implementation session. No cloud/VPS deployment is required.
