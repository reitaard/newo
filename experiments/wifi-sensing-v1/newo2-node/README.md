# Newo2 CSI and ESP-NOW sensing node

This standalone ESP-IDF application targets the validated GOOUUU ESP32-S3-CAM
V1.5 (ESP32-S3-WROOM-1 N16R8). Phase 3 is deliberately radio-only: it does not
initialize the OV3660 camera, microSD, RGB LED, buttons, or any production Newo
peripheral. The validated pin reservations in `docs/newo2-hardware.md` remain
untouched.

It reuses the Phase 2 bounded CSI measurement plane and adds one-way ESP-NOW
probe transmission:

```text
                         normal 2.4 GHz AP
                         /               \
        ROUTER_NEWO     /                 \    ROUTER_NEWO2
                       v                   v
                    Newo <-------------- Newo2
                           NEWO2_NEWO
                    ESP-NOW probes, 20 Hz
```

Both stations first associate with the same AP. The ESP-NOW peer uses
`WIFI_IF_STA` and channel `0`, which means the peer follows the station
interface's current AP channel rather than selecting or hopping channels.
Newo2 pings its DHCP gateway for controlled router replies and sends a separate
version-1 probe to Newo at 20 Hz by default. Both rates are configurable from
1 through the experiment hard maximum of 50 Hz.

Every successful reassociation creates an explicit association epoch. Outside
the event callback, the node refreshes its BSSID, channel state, AP filter,
gateway, and self-ping target before CSI capture resumes. Newo2's
`ROUTER_NEWO2` cadence gate is receiver-local; Newo independently gates its two
accepted sources.

## Required local MAC configuration

Configure credentials, collector address, and **Newo's station MAC** under
`Newo2 CSI and ESP-NOW node` in `idf.py menuconfig`. On Newo, enable the Phase 3
ESP-NOW receive role and configure **Newo2's station MAC**. The MACs are explicit
measurement identities; do not infer either source from packet cadence or path
labels.

The expected path mapping is:

| Receiver | Source MAC | Path |
| --- | --- | --- |
| Newo | AP BSSID | `ROUTER_NEWO` (`1`) |
| Newo2 | AP BSSID | `ROUTER_NEWO2` (`2`) |
| Newo | Newo2 station MAC | `NEWO2_NEWO` (`3`) |

Newo2 does not capture a `NEWO_NEWO2` probe path in Phase 3.

## Build, without flashing

Tested with ESP-IDF v5.5.5 and `espressif/idf:v5.5.5`. From this directory:

The hardened credential-free reference build produced `newo2_csi_node.bin` at
**733,296 bytes (`0xb3070`)**, leaving 30% of the default 1 MiB application
partition free. Size is configuration-dependent.

```sh
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

Credential-free Docker build from the repository root:

```sh
docker run --rm -v "$PWD:/project" \
  -w /project/experiments/wifi-sensing-v1/newo2-node \
  espressif/idf:v5.5.5 bash -lc 'idf.py set-target esp32s3 && idf.py build'
```

`sdkconfig`, build outputs, binaries, and credentials are ignored. An empty SSID
supports secret-free CI compilation but deliberately aborts at runtime. Do not
flash as part of this phase.

Diagnostics once per second report STA association, CSI callback/accepted
rates, CSI and STATUS transport separately, current CSI source/channel, the
association epoch, per-path gate drops, and versioned ESP-NOW attempted,
queued, link-success, link-failure, submit-failure, busy, unassociated, and
receive counters. ESP-NOW link-layer success is useful
coexistence evidence, not proof that every probe produced a CSI callback.

Probes are deliberately unencrypted and contain no credentials or application
secrets. The configured source MAC is an experiment identity/filter, not
cryptographic authentication. Phase 3 also pauses probe submission while STA
association is down and permits only one ESP-NOW send in flight.

See [PROBE_PROTOCOL.md](../PROBE_PROTOCOL.md) for the wire header and
[PROTOCOL.md](../PROTOCOL.md) for CSI records.
