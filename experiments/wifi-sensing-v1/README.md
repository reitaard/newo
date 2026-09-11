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

## Authoritative phase roadmap

1. **Phase 1 — CSI experiment contract/protocol (done):** architecture, NCSI contract, path identifiers, metadata, and safety boundaries.
2. **Phase 2 — standalone Newo CSI receiver (done):** independently validate `ROUTER_NEWO`.
3. **Phase 3 — Newo2 and controlled peer RF path (done):** validate `ROUTER_NEWO`, `ROUTER_NEWO2`, and `NEWO2_NEWO`.
4. **Phase 4 — robust host collector and replayable datasets (done):** strict UDP decoding/CRC, exact raw archival, metadata, replay, diagnostics, and drop accounting. Phase 4.5 hardware validation/coarse analysis is complete experimental groundwork; its tools and results are not Phase 5.
5. **Phase 5 — trustworthy DSP and field/research tooling (software complete, hardware threshold validation pending):** geometry-separated conservative DSP, calibration, replay equivalence, synthetic tests, live research console, and Termux-first portable field monitor. Presence remains unsupported and physical threshold evidence is incomplete.
6. **Phase 6 — cross-node time synchronization (software complete, hardware validation pending):** Newo-led ESP-NOW event-time synchronization, common derived host timeline, measured offset/drift/jitter independent of collector choice, and unattended desired-state recovery.
7. **Phase 7 — ReTrack:** Phase 7A establishes a local-first Linux/Termux runtime, explicit leased LAN control, durable chunked sessions, dynamic node/topology identity, shared live/replay DSP, and a compact four-slot UI over a dynamic backend. Phase 7B calibration/model work is later. Camera teacher experiments remain consented research only and are not a production dependency.
8. **Phase 8 — research sensing models:** held-out evaluation of zone, direction, activity, posture, and count research without capability claims from trainability alone.
9. **Phase 9 — identity/gait research:** rigorous cross-session/placement feasibility work only after earlier stages stabilize.
10. **Phase 10 — respiration research:** lower-frequency research after motion and synchronization are trustworthy; no medical claim.
11. **Phase 11 — link ablation/ESP3 decision:** measure each RF path's contribution before buying or adding nodes.
12. **Phase 12 — production integration:** integrate only validated capabilities, including a mobile web/PWA or equivalent, commissioning/discovery, setup/placement, calibration, diagnostics, and history without research-only assumptions or unsupported claims.

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

## Phase 5 host research tools

Run `python -m newo_csi live` from `tools/` for the dependency-free ANSI
research console. Live UDP and `live --replay <session>` feed the same
streaming DSP pipeline. State is partitioned by path, node, receiver/source
MACs, channel, bandwidth, PHY/LTF description, and CSI payload geometry.
Calibration is separate JSON keyed by that exact identity; a placement,
geometry, or feature-contract change rejects it rather than silently reusing
it. Phase-5 scoring is amplitude-derived; phase remains diagnostic research
evidence pending sanitation and synchronization. Reported states are
only `QUIET`, `RF_CHANGE`, `MOTION_CANDIDATE`, `LOW_CONFIDENCE`, or
`REPOSITIONING`, never person presence or localization.

`python -m newo_csi field` is the portable Phase-5 validation view, not a new
phase or separate sensing implementation. It shares the decoder, DSP,
calibration, archive, metadata, and event model with `live`, and its protocol
and data model are deliberately compatible with a future Phase-12 phone UI.
Collector discovery is specified in [`COLLECTOR_DISCOVERY.md`](./COLLECTOR_DISCOVERY.md)
and implemented on the development sensing targets. CSI remains unicast UDP;
the negligible NCOL announcement plane only selects the collector address.

Fresh Termux setup and start:

```sh
pkg update
pkg install python git
git clone --branch wifi-sensing-phase6-sync-20260910 https://github.com/reitaard/newo.git
cd newo/experiments/wifi-sensing-v1/tools
python -m pip install -e .
mkdir -p ~/.config/newo-csi
python -m newo_csi field --room bedroom
```

To feed the optional VPS Telegram panel, configure the collector process (not
the ESPs) with the server endpoint and its dedicated bearer token:

```sh
export NEWO_TRACK_TELEMETRY_URL=https://newo.example/track/telemetry/v1
export NEWO_TRACK_TELEMETRY_TOKEN='replace-with-vps-secret'
python -m newo_csi field --room bedroom
```

The collector sends one bounded `newo_track_telemetry_v1` derived snapshot at
most every two seconds through a single-latest-slot background worker. Network
failure never blocks UDP collection or archive writing. No NCSI datagram, CSI
I/Q payload, credential, or inferred person/location result is sent. Live and
replay use the same snapshot builder.

An optional `~/.config/newo-csi/field.json` removes repeated arguments:

```json
{
  "schema_version": 1,
  "defaults": {"dataset_dir": "~/newo-csi-data/datasets", "port": 5005},
  "rooms": {"bedroom": {"placement": "BED_SIDE", "scenario": "BACKGROUND"}}
}
```

Command-line values override the named room, then config defaults. The field
view automatically announces itself, restores the terminal on exit, and keeps
the existing `R`, `S`, `E`, and `P` controls. A placement change enters
`REPOSITIONING`, stops the current recording so antenna motion is excluded,
and invalidates calibration.

`evaluate`, `compare`, and `catalog` provide immutable reports and inventory
over existing archives through the same pipeline. Original capture metadata,
append-only post-hoc operator annotations, and DSP inference remain separate.
Trailing partial windows are retained for audit but excluded from aggregate
percentiles and durations.

`python -m newo_csi report SESSION --output-dir reports` writes the stable
`newo_csi_derived_report_v1` JSON contract plus small path CSV and Markdown
artifacts. Raw `frames.ncsi` stays on collector/VPS storage. The report keeps
capture metadata, annotations, transport health, calibration, geometry,
path evidence, and ranked RF-change intervals explicit; its synchronization
field reports the actual Phase-6 model, including session epochs, quality,
offset/drift/residual evidence and stale intervals. Legacy captures report
synchronization unavailable, and the report makes no person/location claim.
The intended boundary is `CSI capture -> immutable dataset -> derived report ->
VPS history -> concise Telegram report`; Telegram never receives raw CSI.

The intended movable-node lifecycle is:

`NEW NODE / MOVED -> REPOSITIONING -> SETTLING -> CALIBRATING -> TRACK_READY`

Moving any sensing node invalidates its geometry/calibration profile. A later
known-profile match must be evidence-based; otherwise a new profile is created.
This is self-settling RF sensor geometry, not room scanning or radar SLAM.

The post-validation `/track` control and real-Newo integration contract is in
[`TRACK_INTEGRATION_PLAN.md`](./TRACK_INTEGRATION_PLAN.md). It keeps measurement
state separate from host recording and does not merge CSI into production
firmware during Phase 5.

## Source lineage

The measurement-plane design adapts a narrow set of implementation ideas reviewed in RuView, including CSI callback configuration, source-MAC filtering, AP channel detection, gateway self-ping, early rate limiting, fixed rings and loss counters, invalid-first-word sanitation, sequence/metadata capture, phase processing concepts, running statistics, and subcarrier selection. RuView is MIT licensed; see [`THIRD_PARTY_NOTICES.md`](./THIRD_PARTY_NOTICES.md). The Newo wire format is independent and is not RuView's wire format.
