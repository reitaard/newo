# Standalone Newo CSI receiver

This ESP-IDF target is the isolated Newo Wi-Fi CSI measurement node. It remains separate from production `Newo/` firmware and performs no DSP, ML, camera work, or production device integration.

Hardware status: **physically validated** on the real Newo ESP32-S3 using ESP-IDF v5.5.5. Router→Newo CSI and Newo2→Newo ESP-NOW peer CSI have both been observed on hardware.

See [`../HARDWARE_VALIDATION_2026-09-10.md`](../HARDWARE_VALIDATION_2026-09-10.md) for measured results.

## Responsibilities

The target:

- associates as a normal 2.4 GHz Wi-Fi station;
- follows the AP channel;
- captures ESP32-S3 CSI in the Wi-Fi callback;
- classifies AP and configured ESP-NOW peer sources explicitly;
- preserves raw CSI geometry;
- zeroes only the invalid leading bytes reported by ESP-IDF;
- enqueues fixed-size records in a bounded static ring;
- sends versioned NCSI CSI/STATUS/DIAGNOSTIC records to the host;
- can generate controlled gateway traffic;
- can receive versioned ESP-NOW probes from Newo2.
- as Newo2, accepts versioned Newo leader sync beacons and emits bounded NCSI
  SYNC evidence without rewriting CSI timestamps.

## Configuration

From this directory:

```sh
idf.py set-target esp32s3
idf.py menuconfig
```

Under **Newo CSI receiver**, configure locally:

- Wi-Fi SSID/password;
- collector IPv4/UDP port;
- node ID `1`;
- router path ID `1` (`ROUTER_NEWO`);
- AP-BSSID source filter;
- gateway ping cadence;
- CSI retention setting;
- ESP-NOW receive role when testing path 3;
- configured Newo2 station MAC for the peer identity.

Generated `sdkconfig`, credentials, builds, captures, and binaries are ignored and must not be committed.

## ESP-NOW role

For the validated three-path topology, Newo uses **ESP-NOW receive** mode. AP-originated CSI remains `ROUTER_NEWO` (`1`); CSI whose source matches the configured Newo2 station MAC is labeled `NEWO2_NEWO` (`3`).

The application probe receive counter and CSI callback counter are deliberately separate. Physical validation showed that peer-originated CSI events can outnumber validated application probes, so peer CSI count must not be treated as application-probe count.

## Rate-gate status

The checked-in reference implementation contains independent receiver-local minimum-spacing gates per path.

During physical validation, a temporary **local worktree edit** bypassed the gate for AP-originated CSI so the actual qualifying router callback rate could be measured. The peer/ESP-NOW path kept its configured gate.

Observed result: with only the AP path active, Router→Newo supplied roughly 24–26 qualifying observations/s in the tested room; the earlier lower retained rate was therefore a gate artifact rather than a compute limit.

That AP bypass is not implied to be present in the checked-in source and is not yet the final retention policy.

## Build

Native ESP-IDF v5.5.5:

```sh
idf.py build
```

Docker from the repository root:

```sh
docker run --rm \
  -v "$PWD:/project" \
  -w /project/experiments/wifi-sensing-v1/newo-rx \
  espressif/idf:v5.5.5 \
  bash -lc 'idf.py set-target esp32s3 && idf.py build'
```

An empty SSID can compile for CI but the runtime will abort until credentials are configured locally.

## Host protocol tests

```sh
cc -std=c11 -Wall -Wextra -Werror \
  -I main host_tests/ncsi_protocol_test.c main/ncsi_protocol.c \
  -o ncsi_protocol_test
./ncsi_protocol_test

cc -std=c11 -Wall -Wextra -Werror \
  -I main host_tests/probe_protocol_test.c \
  main/probe_protocol.c main/ncsi_protocol.c -o probe_protocol_test
./probe_protocol_test

cc -std=c11 -Wall -Wextra -Werror \
  -I main host_tests/rate_gate_test.c main/rate_gate.c -o rate_gate_test
./rate_gate_test

cc -std=c11 -Wall -Wextra -Werror -I main \
  host_tests/sync_protocol_test.c main/sync_protocol.c main/sync_estimator.c \
  main/ncsi_protocol.c -lm -o sync_protocol_test
./sync_protocol_test
```

## Physical flashing boundary

The generated ESP-IDF `flash_args` includes an experiment bootloader and experiment partition table. The validated Newo hardware already has a different production partition layout, so the physical experiment did **not** use full generated flash args.

Before any hardware write, the production flash map was read and backed up. The experiment app was then written **app-only at `0x10000`**, leaving the production bootloader and partition table intact.

Do not copy that procedure to another board without first verifying its real partition layout and security state. Never `erase-flash` merely to run this experiment.

## Runtime diagnostics

Once per second the firmware emits STATUS/DIAGNOSTIC data over UDP and also prints a serial diagnostic line containing:

- callback and accepted rates;
- CSI UDP success/failure;
- ring drops;
- latest RSSI/CSI length/channel/source;
- association state/epoch;
- per-path gate drops;
- ESP-NOW transmit/receive counters.

For long runs, host NCSI diagnostics remain the authoritative machine-readable record; serial logging is an additional debugging record.

## Implementation boundaries

- fixed associated 2.4 GHz channel;
- no channel hopping;
- no DSP in callback;
- no callback heap allocation or network send;
- bounded static SPSC ring;
- raw signed I/Q stays in ESP-IDF imaginary-real order;
- queue/transport/gate loss stays observable;
- no production gesture model yet;
- no medical, fall, identity, person-count, or sleep-stage claims.

See [`../PROTOCOL.md`](../PROTOCOL.md), [`../PROBE_PROTOCOL.md`](../PROBE_PROTOCOL.md), and [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).
