# Standalone Newo CSI receiver

This ESP-IDF application is the Phase 2 measurement-plane receiver. It targets ESP32-S3 and is intentionally isolated from production firmware in `Newo/`. It captures raw CSI, zeroes only the invalid leading bytes reported by ESP-IDF while preserving buffer geometry, and sends version-1 NCSI datagrams to a host collector. It performs no DSP, inference, ML, camera work, or hardware-specific Newo integration.

Tested build toolchain: **ESP-IDF v5.5.5**, using the official `espressif/idf:v5.5.5` Docker image.
The credential-free reference build produced `newo_csi_receiver.bin` at
**704,928 bytes (`0xac1a0`)**, leaving 33% of the default 1 MiB application
partition free. Size is configuration-dependent.

## Configuration

Create local configuration from this directory:

```sh
idf.py set-target esp32s3
idf.py menuconfig
```

Under **Newo CSI receiver**, set the Wi-Fi SSID/password and collector IPv4/UDP port. Also check the node ID, Phase-1 path ID, source filter, raw rate, and gateway-ping rate. The defaults retain and ping at 20 Hz; both settings have a hard Kconfig/build maximum of 50 Hz.

`sdkconfig` is ignored. Do not add it, credentials, captures, binaries, `build/`, `dependencies.lock`, or `managed_components/` to Git. `sdkconfig.defaults` contains only non-secret project defaults.

The default source filter uses the associated AP BSSID. Choose the custom MAC option for a deliberate `NEWO2_NEWO` source. Disabling filtering is diagnostic-only: every datagram still carries the actual callback source MAC and the collector must validate `(receiver_mac, source_mac)` before trusting `path_id`.

## Native build

With ESP-IDF v5.5.5 exported in the current shell:

```sh
idf.py set-target esp32s3
idf.py build
```

An empty SSID is accepted at compile time so CI can build the complete runtime without secrets, but the firmware stops at boot and asks for menuconfig configuration. A build is not hardware validation.

## Reproducible Docker build

From the repository root on Linux/macOS:

```sh
docker run --rm \
  -v "$PWD:/project" \
  -w /project/experiments/wifi-sensing-v1/newo-rx \
  espressif/idf:v5.5.5 \
  bash -lc 'idf.py set-target esp32s3 && idf.py build'
```

PowerShell:

```powershell
docker run --rm `
  -v "${PWD}:/project" `
  -w /project/experiments/wifi-sensing-v1/newo-rx `
  espressif/idf:v5.5.5 `
  bash -lc 'idf.py set-target esp32s3 && idf.py build'
```

The container writes ignored `sdkconfig` and `build/` outputs into the experiment directory. Delete or retain them locally as needed; never commit them.

## Host protocol test

The serializer is dependency-free C. With a host C compiler:

```sh
cc -std=c11 -Wall -Wextra -Werror \
  -I main host_tests/ncsi_protocol_test.c main/ncsi_protocol.c \
  -o ncsi_protocol_test
./ncsi_protocol_test
```

The test checks CRC-32C, fixed offsets, lengths, I/Q geometry-preserving sanitation, payload identity, and rejection of odd I/Q lengths.

## Run without flashing instructions

Phase 2 does not authorize flashing. When a later phase explicitly authorizes hardware use, configure a collector first and use the normal ESP-IDF monitor workflow. UDP datagrams contain one NCSI record each. The firmware also prints one diagnostic line per second with callback/accepted rate, cumulative UDP results and queue drops, and the latest RSSI, CSI length, channel, and source MAC.

Gateway self-ping discovers the DHCP gateway through the station network interface and sends one-byte ICMP echo requests. Router replies are ordinary controlled traffic; no special router firmware is required. The `CONTROL_TRAFFIC_ACTIVE` flag means the generator was running, not that a particular CSI callback was certainly caused by its reply.

## Implementation boundaries

- fixed 2.4 GHz associated channel; no channel hopping or 5 GHz logic;
- ESP32-S3 LLTF, HT-LTF, and STBC HT-LTF capture only;
- no per-callback heap allocation, logging, UDP, CRC, or DSP;
- statically allocated single-producer/single-consumer ring;
- source filtering precedes the processing cadence gate;
- full/invalid rings and transport failures are counted;
- raw signed I/Q byte pairs remain in ESP-IDF imaginary-real order;
- no C6, NDP, time-sync, WASM, Home Assistant, pose, heartbeat, person count, or ML code.

See the parent [PROTOCOL.md](../PROTOCOL.md) and [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) for the byte format and RuView MIT attribution.
