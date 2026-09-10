# Newo CSI host tools

Python 3.10+ tooling for capturing, inspecting, replaying, and optionally
exporting Newo CSI protocol v1. The canonical capture is not CSV:

```text
datasets/<session_id>/
  session.json    immutable labels plus capture start/end
  events.jsonl    host capture lifecycle events
  frames.ncsi     append-only binary envelopes containing exact UDP records
  summary.json    final counts, rates, RF geometry, and loss diagnostics
```

Each 36-byte version-2 `NCAP` envelope stores authoritative host monotonic
receive nanoseconds, separate wall-clock nanoseconds, sender IPv4/port, and
record length followed by the original NCSI datagram without re-encoding it.
The strict decoder validates magic, version, lengths, CSI geometry,
first-word sanitation, I/Q order, and CRC-32C before archival.
The collector requests a 4 MiB UDP receive buffer by default and records the
effective OS value in session metadata; sequence/status counters remain the
authoritative indicators of loss.

The little-endian NCAP v2 envelope layout is: magic `NCAP` (4 bytes), version
`2` (1), flags `0` (1), header size `36` (2), host monotonic ns (8), host wall
ns (8), embedded record length (4), sender IPv4 (4), sender UDP port (2), and
reserved zero (2). v1 archives are intentionally rejected: no physical
datasets existed when monotonic timing became mandatory, so silent conversion
of the old wall-time-only field would be misleading.

Run without installation from this directory:

```sh
python -m newo_csi collect --room-id ROOM_A --scenario EMPTY --duration 60 \
  --placement DOOR_LEFT \
  --router-bssid aa:bb:cc:dd:ee:ff \
  --newo-mac 10:11:12:13:14:15 --newo2-mac 20:21:22:23:24:25
python -m newo_csi inspect datasets/<session_id>
python -m newo_csi replay datasets/<session_id> --host 127.0.0.1 --port 5005
python -m newo_csi export datasets/<session_id> --output inspection-export.csv \
  --subcarriers 8,16,24
python -m newo_csi live --room-id ROOM_A --placement DOOR_LEFT
python -m newo_csi live --calibrate 30 --room-id ROOM_A --placement DOOR_LEFT
python -m newo_csi live --replay datasets/<session_id> --speed 0
python -m newo_csi field --room-id ROOM_A --placement DOOR_LEFT
python -m newo_csi evaluate datasets/<session_id> --window-seconds 1 --json
python -m newo_csi compare datasets/<session_a> datasets/<session_b> --window-seconds 1
python -m newo_csi catalog datasets --calibration-file calibrations/baseline.json
python -m newo_csi annotate datasets/<session_id> --label FAR_CORNER_LIGHT_ACTIVITY \
  --note "one seated near Newo; one doing light activity in far corner"
python -m unittest discover -s tests -v
```

The live console uses ANSI redraw and single-key controls: `R` starts a new
exact `frames.ncsi` recording, `S` stops it, `E` writes a marker, `P` changes
the user-defined Newo2 placement, `O`/`A` change occupancy/activity labels,
and `Q` quits. Markers include `DOOR_OPEN`, `DOOR_CLOSE`, `ENTER`, `EXIT`,
`WALK`, `WAVE`, and `CUSTOM`. Label changes are appended to `events.jsonl`;
the original NCSI datagrams are never modified.

Changing placement automatically closes an active recording, enters
`REPOSITIONING` for the configured settling time, clears active calibration,
and requires a new recording after the antenna is stable. This prevents the
sensing-node movement itself from entering a human-movement dataset.

The DSP computes amplitude and wrapped phase from signed imaginary/real pairs,
unwraps each subcarrier over time, maintains Welford mean/variance, ranks
subcarriers during calibration warm-up, freezes the top-K indices and amplitude
normalization, then computes timestamp-scaled amplitude derivatives and
windowed power. Phase remains observable diagnostic evidence but has zero
weight in the Phase-5 score pending common-phase/CFO sanitation and time
synchronization validation. Espressif's phase-coherent architecture uses
shared-clock hardware specifically to eliminate relative frequency offset;
independent receivers do not provide that guarantee. See Espressif's
[ESP-CSI solution architecture](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/solution-introduction/esp-csi/esp-csi-solution.html).

RSSI contributes only to link quality, never directly to motion. Motion scores
exist only when exact path/geometry and feature-contract calibration is
available. Calibration schema v3 records feature schema, top-K, window length,
derivative/gap/normalization policy, frozen indices/scales, and baseline power
statistics. `--calibrate N` uses the first 40% for selection and remaining 60%
for baseline measurement. Older calibration documents remain readable but fail
closed because their scoring dimensions are unknown. Since quiet calibration
alone cannot validate stationary occupancy, presence remains unsupported
rather than being reported as a fact.

## Termux field monitor

Android/Termux is the primary portable target. In Termux:

```sh
pkg update
pkg install python git
cd /path/to/newo/experiments/wifi-sensing-v1/tools
python -m newo_csi field --room-id ROOM_A --placement DOOR_LEFT
```

No Python packages beyond the standard library are required. Keep the phone on
the same 2.4 GHz LAN and allow Termux to run in the foreground while recording.
Until the separately documented collector-discovery design is implemented in
experiment firmware, configure the nodes' collector destination to the phone's
current Wi-Fi IPv4 address and UDP port 5005. AP client isolation, Android VPNs,
or firewall policy can block local UDP.

The portrait view shortens path names and columns below 50 terminal columns.
It shows both nodes, UDP state, all paths, measured Hz, RSSI, link quality,
receiver-wide sequence loss, conservative path/fused state, calibration,
recording elapsed time, placement, and operator occupancy/activity labels.
Controls are `R` record, `S` stop, `E` marker, `P` placement, `O` occupancy,
`A` activity, and `Q` quit. It writes the same session directory and exact
`frames.ncsi` envelopes as the desktop console.

iSH/iOS is best-effort only: direct background/local-network UDP and terminal
input behavior may vary. The shared host state and binary protocol are kept
independent of terminal rendering so a future HTTP/WebSocket/PWA view can sit
above the same decoder and DSP instead of reimplementing them.

## Offline Phase-5 evaluation

`evaluate` reads existing archives without modifying them and sends every CSI
record through the same `CsiPipeline` used by `live`, `field`, and replay.
Supply the exact calibration document associated with the recorded placement
when calibrated scores are required:

```sh
python -m newo_csi evaluate datasets/session-a datasets/session-b \
  --window-seconds 1 --top-k 24 --calibration-file calibrations/baseline.json --json
python -m newo_csi compare datasets/session-a datasets/session-b \
  --window-seconds 2
```

Calibration output explains the recorded room and placement, calibration
timestamp/age, exact path/MAC/channel/bandwidth/PHY/LTF/CSI geometry identities,
matched paths, and rejection reasons. Missing placement, room mismatch,
placement mismatch, or geometry mismatch fails closed: scores stay unavailable
and state remains `LOW_CONFIDENCE`. Age is reported for operator review; v1 does
not invent an unsupported expiry threshold. JSON includes timestamped windows,
per-path score/state/rate/RSSI/quality, score percentiles, state fractions,
continuous-change durations, one/two/three-path patterns, link degradation,
geometry transitions, receiver sequence gaps, device drop counters, saved
session labels, and event annotations. Operator labels stay separate from
inference and are never treated as proof of detection.

Only complete evaluation windows contribute to percentiles, state fractions,
agreement durations, or degradation durations. A final short window is retained
as `partial: true` for inspection but marked `included_in_aggregate: false`.

`catalog` streams each immutable archive to validate its envelopes/decoder and
lists duration, original labels, post-hoc annotations, paths present/missing,
sequence/device loss, calibration compatibility, and raw readability. It is
read-only; use `--json` for a machine-readable inventory. Archives created
before `placement_label` was introduced remain readable and are explicitly
marked as predating placement metadata rather than having a placement guessed.

Known nuisance classes such as speaker music, fan airflow, curtain movement,
device vibration, and background activity must be explicitly recorded in
future sessions. The evaluator reports RF patterns; it does not infer which
nuisance or person caused them. See
[`../TRANSPORT_AUDIT.md`](../TRANSPORT_AUDIT.md) for the static
`device_transport_drops` analysis and proposed instrumentation.

## Post-hoc operator annotations

`annotate` never rewrites a dataset. It appends one versioned JSON object to
`operator-annotations/<session-id>.jsonl`, outside the immutable session
directory and ignored by Git because the labels are privacy-sensitive evidence.
Repeated annotations form a correction history; later entries do not delete or
replace earlier statements.

Whole-session annotation:

```sh
python -m newo_csi annotate datasets/<session-id> \
  --label FAR_CORNER_LIGHT_ACTIVITY \
  --note "one seated near Newo; one doing light activity in far corner"
```

Explicit future elapsed-time range:

```sh
python -m newo_csi annotate datasets/<session-id> \
  --start 12.5 --end 18.0 --label WALK_PASS
```

Both range endpoints are required and must satisfy `0 <= start < end`; the tool
never guesses timing. Evaluation output presents `ORIGINAL CAPTURE METADATA`,
`OPERATOR POST-HOC ANNOTATION`, and `DSP INFERENCE` independently.

Use `--speed 1` for recorded replay timing, `--speed 2` for twice real time, or
`--speed 0` for no delays. Replay sends the exact embedded NCSI records as UDP
datagrams and derives delays only from monotonic capture timestamps, so host
wall-clock corrections cannot distort RF timing. Wall time is retained for
human chronology. CSV export is derived inspection data only; it emits signed
imaginary/real bytes, amplitude, and wrapped phase for selected subcarriers.
It must not replace `frames.ncsi`.

Sequence loss is calculated across each receiver's node-ID/MAC stream because
device CSI sequence numbers are node-local and may interleave paths. Status
records separately expose intentional per-path rate gating, source filtering,
ring-full drops, and CSI-only device UDP failures. Diagnostic records expose
STATUS transport, association epochs, per-path gate drops, and ESP-NOW probe
outcomes; diagnostic sequence gaps reveal diagnostic-record loss.

Live and field views show `RX Newo` and `RX Newo2` gaps separately; receiver
loss is never summed across path rows. Evaluator `csi_geometry_variants` and
`csi_geometry_switch_count` describe CSI record/radio-format identities, not
physical node movement. Physical movement exists only in `placement_label` and
explicit `REPOSITIONING` events.

Capture metadata schema v3 is shared by `collect`, live, and field recording.
`occupancy_label` is canonical; `person_label` is retained as an identical
compatibility alias. Existing archives are read without rewriting.

For reproducible research captures, supply all three MAC options shown above.
They validate the receiver/source tuple for `ROUTER_NEWO`, `ROUTER_NEWO2`, and
`NEWO2_NEWO`; mismatches are rejected and counted. Omitting all three is useful
for initial diagnostics but trusts the transmitted path ID and records a null
mapping in session metadata.
