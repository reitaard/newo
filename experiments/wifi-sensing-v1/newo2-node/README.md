# Newo2 CSI and ESP-NOW sensing node

This standalone ESP-IDF target runs the radio-only side of the Wi-Fi sensing experiment on the GOOUUU ESP32-S3-CAM Newo2 hardware. It does not initialize the OV3660 camera, microSD, RGB LED, buttons, or other production Newo2 peripherals.

Hardware status: **physically validated** on the real Newo2 ESP32-S3. Router→Newo2 CSI and Newo2→Newo ESP-NOW transmission have both been exercised on hardware while Newo2 remained associated to the same AP as Newo.

See [`../HARDWARE_VALIDATION_2026-09-10.md`](../HARDWARE_VALIDATION_2026-09-10.md) for measured results.

## Validated topology

```text
                         fixed 2.4 GHz AP
                         /              \
        ROUTER_NEWO     /                \   ROUTER_NEWO2
                       v                  v
                    Newo <------------- Newo2
                           NEWO2_NEWO
                         ESP-NOW probes
```

Both stations remain ordinary Wi-Fi stations. The ESP-NOW peer uses `WIFI_IF_STA` and channel `0`, so it follows the associated AP channel rather than hopping independently.

## Responsibilities

Newo2:

- associates to the same fixed 2.4 GHz AP as Newo;
- captures AP-originated CSI as `ROUTER_NEWO2` (`2`);
- generates controlled gateway traffic;
- optionally transmits versioned ESP-NOW probes to Newo;
- emits CSI, STATUS, and DIAGNOSTIC records to the host collector;
- keeps camera/SD/RGB hardware unused in this experiment target.

## Configuration

From this directory:

```sh
idf.py set-target esp32s3
idf.py menuconfig
```

Under **Newo2 CSI and ESP-NOW node**, configure locally:

- Wi-Fi SSID/password;
- node ID `2`;
- router path ID `2` (`ROUTER_NEWO2`);
- collector IPv4/UDP port;
- AP-BSSID source filter;
- gateway ping cadence;
- CSI retention setting;
- ESP-NOW transmit role for path 3;
- Newo station MAC as the peer identity;
- ESP-NOW probe cadence.

Generated `sdkconfig`, credentials, builds, captures, and binaries are ignored and must not be committed.

## ESP-NOW validation

The first physical three-path run used a configured **20 Hz application probe schedule**.

Over 60 s, Newo2 reported:

- 1,079 transmit attempts;
- 1,079 queued sends;
- 1,079 link successes;
- 0 link failures;
- 0 submit failures;
- 131 skipped-busy schedule slots;
- 0 skipped-unassociated slots.

Newo reported exactly 1,079 valid application probes received and zero invalid probes.

This means application-level delivery was complete for every queued/link-successful probe in that run. The effective successful application-probe cadence was about 18 Hz because some nominal 20 Hz schedule slots were skipped while a previous send was still in flight.

Peer CSI count is **not** one-to-one with application probes. The same run produced 1,855 `NEWO2_NEWO` CSI records (30.9 Hz) plus additional peer-path gate drops on Newo. Use `probe_tx_*`/`probe_rx_*` diagnostics for application-probe delivery, not CSI frame count.

## Rate-gate status

The shared checked-in receiver source contains the original independent minimum-spacing gate. During physical validation, a temporary local edit bypassed the AP-originated gate while leaving the ESP-NOW peer path gated. This allowed the actual AP callback rate to be measured.

That bypass is not implied to be part of the checked-in source and is not yet the final retention policy.

## Build

Native ESP-IDF v5.5.5:

```sh
idf.py build
```

Docker from the repository root:

```sh
docker run --rm \
  -v "$PWD:/project" \
  -w /project/experiments/wifi-sensing-v1/newo2-node \
  espressif/idf:v5.5.5 \
  bash -lc 'idf.py set-target esp32s3 && idf.py build'
```

The Newo2 target reuses the shared receiver/protocol implementation from `newo-rx` with Newo2-specific Kconfig defaults and ESP-NOW transmit configuration.

## Physical flashing boundary

The validated Newo2 production flash layout contains a production bootloader, NVS/PHY partitions, and an 8 MiB factory app partition beginning at `0x10000`.

The generated experiment `flash_args` contains its own bootloader and experiment partition table. Physical validation therefore used an **app-only write at `0x10000`** after the real Newo2 flash layout and factory application were backed up. The production bootloader and partition table were left intact.

Do not blindly use full generated `@flash_args` on a production-configured Newo2. Verify the real target layout first, and do not `erase-flash` just to run this experiment.

## Power-bank operation

After the experiment app is flashed, Newo2 does not require USB/serial to participate. It can be placed at a fixed room position and powered from a power bank while sending CSI/STATUS/DIAGNOSTIC records over Wi-Fi and ESP-NOW probes to Newo.

This is the preferred geometry for long room captures when keeping Newo2 next to the collector laptop would reduce spatial diversity.

When serial is unavailable, host NCSI diagnostics still expose association epoch, CSI counters, transport loss, and ESP-NOW transmit outcomes.

## Implementation boundaries

- fixed associated 2.4 GHz channel;
- no camera/SD/RGB initialization;
- no channel hopping;
- no DSP/ML in the device callback;
- bounded static CSI ring;
- explicit receiver/source identities;
- raw I/Q preservation;
- queue/transport/gate losses remain observable;
- no gesture classifier on-device yet;
- no medical, identity, person-count, or sleep-stage claims.

See [`../PROBE_PROTOCOL.md`](../PROBE_PROTOCOL.md), [`../PROTOCOL.md`](../PROTOCOL.md), and [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).
