# Newo Wi-Fi CSI sensing experiment v1

Status: **physical hardware validation is active**. The standalone ESP-IDF targets under [`newo-rx/`](./newo-rx/) and [`newo2-node/`](./newo2-node/) have been exercised on the real Newo and Newo2 ESP32-S3 boards. Two router CSI paths and the Newo2→Newo ESP-NOW path have been observed simultaneously. Production `Newo/` firmware is still isolated from this experiment, the Newo2 camera remains disabled in the sensing target, and raw datasets remain outside Git.

See [`HARDWARE_VALIDATION_2026-09-10.md`](./HARDWARE_VALIDATION_2026-09-10.md) for measured rates, loss counters, geometry observations, and the first three-path ESP-NOW result.

## Goal

Use two fixed ESP32-S3 devices and an ordinary fixed 2.4 GHz router as a room-scale RF sensing system, with a practical end goal of **reliable learned gestures that can trigger Newo actions**.

The experiment may also support later, separately validated research into presence, movement, direction, activity, respiration, and other RF features. Retaining CSI does not prove that a downstream task is feasible or accurate. Health, safety, identity, sleep-stage, or person-specific claims require their own validation and are not implied by this experiment.

## Hardware

- **Newo**: ESP32-S3, 16 MB flash, 8 MB PSRAM.
- **Newo2**: GOOUUU ESP32-S3-CAM, ESP32-S3, 16 MB flash, 8 MB PSRAM, OV3660 camera and microSD available to production firmware but unused by the radio-only sensing target.
- **Router**: ordinary fixed 2.4 GHz access point.
- **Host collector**: receives versioned NCSI UDP records and stores exact raw datagrams with host monotonic timestamps.

## Validated topology

```text
                         fixed 2.4 GHz router
                        /                    \
             ROUTER_NEWO                      ROUTER_NEWO2
                      /                        \
                  Newo  <---- NEWO2_NEWO ---- Newo2
                    |                              |
                    +----- versioned NCSI records-+
                                      |
                                host collector
                                      |
                    session metadata + frames.ncsi
                                      |
                          offline geometry-aware DSP
                                      |
                         gesture/background studies
```

All three paths were observed on the same associated AP channel:

| Path ID | Receiver | Source identity | Purpose |
| --- | --- | --- | --- |
| `ROUTER_NEWO` (`1`) | Newo | AP BSSID | Router traffic, including controlled gateway replies |
| `ROUTER_NEWO2` (`2`) | Newo2 | AP BSSID | Router traffic, including controlled gateway replies |
| `NEWO2_NEWO` (`3`) | Newo | configured Newo2 station MAC | deliberate ESP-NOW peer traffic |

Path identity is based on explicit receiver/source MAC configuration, never inferred from timing, RSSI, proximity, or packet cadence.

## Measurement-plane rules

The firmware keeps the Wi-Fi callback small: validate metadata, classify the source, copy CSI into a bounded static ring, and return. Allocation, UDP transport, logging, DSP, and storage stay outside the callback.

Raw CSI remains signed 8-bit complex I/Q in ESP-IDF imaginary-real byte order. When ESP-IDF reports `first_word_invalid`, only the invalid leading bytes are zeroed in place; payload length and subcarrier geometry are preserved.

After every association, AP BSSID/channel/filter/gateway identity is refreshed before CSI acceptance resumes. ESP-NOW uses the station interface and follows the AP channel rather than hopping independently.

Every accepted CSI record carries receiver/source identity, path, sequence, local timestamp, RSSI, channel, receive metadata, CSI geometry, and loss-observable counters. Periodic STATUS and DIAGNOSTIC records expose queue, transport, association, per-path gating, and ESP-NOW probe behavior.

## Important hardware findings

The first physical validation changed several assumptions from the design-only phase:

- A single Router→Newo path produced roughly **24–26 qualifying observations/s** when the local AP-path minimum-spacing gate was bypassed.
- The earlier lower retained rate was caused by software gate semantics, not ESP32-S3 processing capacity.
- With both stations generating 20 Hz gateway traffic, the two router paths produced roughly **43–45 CSI frames/s each** in repeated five-minute busy-room captures.
- The higher packet rate came from additional controlled traffic, so **packet count/cadence is not a movement feature**.
- Both receivers sustained the tested load with **zero ring overflow**.
- AP CSI was dominated by 612-byte geometry, with 376-byte frames and occasional 128/256-byte variants. Geometry classes must not be mixed blindly in DSP.
- A 60 s ESP-NOW validation produced all three expected paths.
- Newo2 queued 1,079 application probes and reported 1,079 link successes; Newo received 1,079 valid probes and zero invalid probes.
- `NEWO2_NEWO` produced more CSI callbacks than validated application probes, so **peer CSI count is not one-to-one with probe count**. Probe delivery must be read from explicit probe diagnostics.
- UDP transport loss is small but non-zero and remains visible through sequence/status counters.

The detailed numbers and caveats are in [`HARDWARE_VALIDATION_2026-09-10.md`](./HARDWARE_VALIDATION_2026-09-10.md).

## Current local hardware-validation variant

The repository reference implementation still contains the original independent per-path minimum-spacing gate. During physical validation, a **temporary local worktree edit** bypassed that gate for AP-originated Router→Newo/Router→Newo2 CSI while retaining the configured gate for the ESP-NOW peer path. This allowed measurement of the actual qualifying AP callback rate.

That local edit is evidence-gathering, not yet the final retention policy. Do not assume the checked-in source already contains the bypass.

## Controlled traffic

Each station can ping its DHCP gateway at a configured rate. The resulting AP replies provide ordinary receive-side traffic without router firmware changes. `CONTROL_TRAFFIC_ACTIVE` only says the generator was running; an individual AP CSI record is not asserted to be a particular ICMP reply.

Newo2 can additionally transmit versioned ESP-NOW probes to Newo. The application probe scheduler, link callback, receive validator, and CSI callback are measured separately. This distinction matters because a single application probe schedule can correspond to more peer-originated Wi-Fi/CSI events than application payload receives.

See [`PROBE_PROTOCOL.md`](./PROBE_PROTOCOL.md).

## Capture format

The Python tooling under [`tools/`](./tools/) stores:

```text
datasets/<session_id>/
  session.json
  events.jsonl
  frames.ncsi
  summary.json
```

`frames.ncsi` is the canonical raw archive: exact validated NCSI datagrams wrapped with authoritative host monotonic and wall-clock receive timestamps. CSV export is derived inspection data only and must not replace the raw archive.

See [`RECORDING.md`](./RECORDING.md) before human-labeled captures.

## DSP direction

Offline extraction should start conservatively:

1. partition by path, receiver/source identity, channel, bandwidth, PHY/LTF metadata, and CSI length;
2. derive amplitude and wrapped phase from raw I/Q;
3. unwrap/filter only within stable geometry classes;
4. measure short-window amplitude variance, phase change, motion energy, and cross-path agreement;
5. retain actual cadence and sequence gaps instead of assuming uniform samples;
6. first prove **still vs walking vs one repeated gesture**;
7. use busy-room and overnight resting sessions as negative/background data;
8. add gesture classes only after held-out repetitions remain separable.

The target is a robust gesture trigger, not a packet-rate heuristic. Mean RSSI and packet count alone are insufficient.

## Recording strategy

A perfectly empty room is not required if the real environment cannot provide one. The baseline must instead be **honestly labeled and repeatable**. For example, a stationary occupied baseline with idle phones/laptops is valid background data when that reflects the deployment environment.

For gesture development, prefer many short independently labeled repetitions over one long mixed session. Long natural-motion and overnight resting sessions are useful as negative/background data and false-trigger tests.

The first planned overnight capture uses two people sleeping/resting naturally, Newo fixed on USB for serial logging, and Newo2 fixed separately on a power bank. This is background RF data, **not validated sleep-stage data**, and with two people the signal must not be assumed to identify which person moved.

## Flashing boundary

The standalone experiment uses its own ESP-IDF build artifacts and partition table, but the validated physical procedure deliberately preserved each device's production bootloader and production partition layout. Experiment binaries were flashed **app-only at `0x10000`** after the actual production layouts were read and backed up.

Do not blindly use generated `@flash_args` on a production-configured board: it includes an experiment bootloader and experiment partition table. Verify the target's real partition map first. Never commit device backups, credentials, generated `sdkconfig`, raw captures, or binaries.

## Roadmap

1. **Protocol/design** — complete enough for hardware work.
2. **Isolated receiver firmware** — physically validated.
3. **Newo2 ESP-NOW third path** — physically validated at 20 Hz application schedule.
4. **Host collector/replay** — physically validated on multi-minute captures.
5. **Long background capture** — overnight two-person resting/background run.
6. **Geometry-aware offline extraction** — next engineering focus.
7. **Gesture feasibility** — still/walk/one-gesture first, then additional learned triggers.
8. **Synchronization/calibration improvements** — add only where cross-node analysis requires them.
9. **Production integration** — only after false-trigger behavior and resource cost are understood.

## Privacy and safety

CSI and labels can reveal occupancy or behavior. Obtain consent, minimize retention, keep raw datasets outside Git, and define deletion rules. Camera media is not embedded in NCSI records; optional camera labels must remain a separate consented data source.

No result in this experiment is a medical measurement, safety detector, identity guarantee, or sleep-stage classifier.

## Build targets

Both experiment targets are tested with ESP-IDF v5.5.5 and the official `espressif/idf:v5.5.5` Docker image:

```sh
cd newo-rx
idf.py set-target esp32s3
idf.py menuconfig
idf.py build

cd ../newo2-node
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

Secret-bearing `sdkconfig` and generated build outputs are ignored and must stay local.

See [`newo-rx/README.md`](./newo-rx/README.md) and [`newo2-node/README.md`](./newo2-node/README.md) for node-specific configuration.

## Source lineage

The measurement-plane design adapts a narrow set of implementation ideas reviewed in RuView, including CSI callback configuration, source-MAC filtering, AP channel detection, gateway self-ping, early rate limiting, fixed rings and loss counters, invalid-first-word sanitation, sequence/metadata capture, phase processing concepts, running statistics, and subcarrier selection. RuView is MIT licensed; see [`THIRD_PARTY_NOTICES.md`](./THIRD_PARTY_NOTICES.md). The Newo wire format is independent and is not RuView's wire format.
