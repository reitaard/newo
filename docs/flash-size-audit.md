# Firmware flash-size audit — 2026-09-06

Baseline: `fa14566`, Arduino-ESP32 3.3.10, `build/usb-uac-3.3.10/Newo.ino.elf` and matching linker map. The 56 copied C/C++/header files match current sources after removing Arduino-generated `#line` directives. No hardware was accessed.

## Baseline

- Reported application size: **2,876,243 bytes**, **91.43%** of a 3,145,728-byte slot; **269,485 bytes (263.2 KiB)** remain.
- Static DRAM: **83,480 bytes** = 39,720 initialized data + 43,760 BSS. This is not peak runtime RAM consumption; heap, task stacks, DMA, TLS and PSRAM allocations require runtime measurement.
- Board flash is 16 MiB. The partition table reserves two 3 MiB application slots, 6 MiB SPIFFS, and a separate 0x3E0000-byte speech-model partition.
- ELF debug sections, linker-map text and build-directory disk usage are not flashed application bytes. The separate model image is not part of the 91% application figure.

## Ranked opportunities

### 1. Exclude unused MultiNet English command recognition

`Newo/newo_audio.cpp:57` calls `ESP_SR.begin(..., nullptr, 0, ..., SR_MODE_WAKEWORD, "MN")`: zero command phrases. However, the installed `ESP_SR/src/esp32-hal-sr.c:358` compiles MultiNet initialization and Flite grapheme-to-phoneme processing unless `CONFIG_SR_MN_EN_NONE` is defined. The packaged SDK selects English MultiNet7. Runtime wakeword mode and zero commands do not remove these references from the linker graph.

The baseline map attributes approximately **613,155 bytes (598.8 KiB)** to `libflite_g2p.a`. Its two largest symbols alone are `cmu_lex_data` (410,522 bytes) and `cmu_lts_model` (153,030 bytes). `libmultinet.a` adds approximately 40,254 bytes. These are linked footprints, not independently measured removal deltas.

The narrow first candidate is a reproducible wakeword-only build profile that defines `CONFIG_SR_MN_EN_NONE` while compiling the Arduino speech C wrapper. Keep the explicit app partition and existing manual voice, Opus, display, USB and BLE behavior. Command-recognition mode would no longer be supported. This is an Arduino wrapper compile-time experiment, not a rebuild of the packaged ESP-IDF libraries; a maintained build profile should document this distinction and pin the core version.

Experimental build: `build/flash-size-audit-no-multinet`, with the baseline FQBN and `--build-property compiler.c.extra_flags=-DCONFIG_SR_MN_EN_NONE=1`. Results are recorded below after completion. It has not been flashed or physically validated.

### 2. Optional manual-voice-only profile

README describes WakeNet as dormant future infrastructure; `/v` directly streams I2S capture. A compile-time profile excluding ESP_SR entirely could remove more speech processing and inference dependencies. The baseline also links approximately 224,701 bytes of `libdl_lib.a`, 83,573 of `libesp_audio_processor.a`, 20,347 of `libwakenet.a`, and 14,637 of `libesp_audio_front_end.a`.

This overlaps opportunity 1; do not add all estimates as guaranteed savings. Legacy/future ON/TOGGLE controls still reach `setEnabled()` and ARMED in `Newo/Newo.ino:108`, so disabling speech recognition needs explicit unsupported-command handling. Preserve the I2S/manual streaming path and speaker suppression lifecycle. Merely keeping voice OFF at runtime does not shrink flash.

### 3. Remove unused built-in certificate bundle while preserving explicit CA validation

The baseline links **68,987 bytes (67.4 KiB)** from `x509_crt_bundle.S.obj`. All three Newo WebSocket channels use `beginSslWithCA`. The map shows `NetworkClientSecure/ssl_client.cpp.o -> esp_crt_bundle_attach -> x509_crt_bundle`.

This requires an isolated, maintained NetworkClientSecure/core configuration change and a before/after link check. Application-level CA selection alone has not removed the bundle. Retain explicit trust anchors and hostname verification; never substitute an insecure TLS connection. Validate all three WSS channels and reconnect behavior before adopting. Savings are a candidate footprint, not a tested delta.

### 4. Share the duplicated display font

`newo_display.cpp` and `newo_display_pose.cpp` both include the defining `FreeSans9pt7b.h` header. The ELF contains two bitmap symbols (1,150 bytes each) and two glyph arrays (760 bytes each). Moving the definition into one translation unit and exposing a shared font accessor/reference should recover approximately **1,910 bytes**, plus a small descriptor/alignment amount, without changing visuals.

All linked font bitmap/glyph arrays total just **15,062 bytes**. Removing display expressions or redesigning the renderer is therefore a poor first response to this flash pressure.

### 5. Optional USB-audio diagnostic build profile

`newo_usb_audio.cpp` always builds experimental mic/tone/duplex diagnostics, and the UAC driver is initialized at startup. The app UAC source, descriptor parser and class-driver objects collectively occupy roughly **28 KiB** before accounting for shared dependencies, string merging and link changes. Compile these out only in a profile that deliberately omits UAC testing; keep MSC and its shared USB host functional. Confirm the actual delta with an isolated build. Disabling an active feature is a tradeoff, not a transparent optimization.

## Lower-priority changes and non-solutions

- **BLE:** `libbt.a` plus `libbtdm_app.a` account for roughly 165 KiB, but BLE is the implemented provisioning path. NimBLE is already enabled. Removing BLE requires replacement provisioning/recovery behavior; releasing Bluetooth RAM after provisioning does not remove its flash code.
- **Opus:** the linked Arduino Opus library is roughly 77 KiB and supports the current speaker transport. Keep it unless deliberately changing the transport and accepting the network tradeoff.
- **Compiler/log settings:** `-Os`, `--gc-sections`, Arduino DebugLevel=none and IDF error-level logging are already in effect. The SDK linker flags explicitly contain `-fno-lto`; LTO needs a coherent build experiment, not a promise from adding one flag.
- **Assertions/log text:** a rebuilt SDK with silent assertions and narrower component logging may save space, but involves toolchain maintenance and reduced diagnostics. No delta measured. See [Espressif's binary-size guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-guides/performance/size.html).
- **Buffers and PSRAM:** reducing audio buffers or moving them into PSRAM primarily affects RAM. It does not address the dominant linked speech tables.
- **Larger app partitions:** storage currently uses NVS and external USB; no firmware SPIFFS mount was found. A custom partition layout could borrow from the 6 MiB SPIFFS reservation while retaining model space and OTA slots. This creates headroom rather than reducing the binary. Treat partition migration as a separate operation because changing offsets affects stored data/model placement and upload tooling.

## Measurement limits

Library footprint estimates come from allocated code/read-only-data/initialized-data input sections in the existing linker map. Linker string merging, alignment and shared dependencies mean these are prioritization figures rather than additive savings. Experimental builds provide stronger before/after evidence. No device behavior or free runtime memory has been verified in this audit.
