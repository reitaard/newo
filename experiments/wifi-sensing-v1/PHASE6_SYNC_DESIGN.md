# Phase 6 event-time synchronization design

Status: software implementation; two-board hardware validation pending.

Newo is the configured leader/reference and Newo2 is a follower. While
tracking is active, Newo sends a 36-byte CRC-protected `NSYN` ESP-NOW beacon
every 500 ms through the existing shared peer-radio owner. The beacon carries a
random, non-persistent leader boot/session epoch, sequence, node identity and
the leader monotonic send timestamp. Newo2 timestamps receipt with its own
monotonic clock, maintains raw offset, an alpha-1/8 offset EMA, residual jitter,
and a drift estimate after eight samples. The estimator is intentionally
one-way and therefore supports event alignment, not RF phase coherence.

Quality is evidence-bound rather than an unmeasured precision claim:

- `UNSYNCED`: no accepted sample.
- `SYNC_WARMING`: fewer than eight samples in the current session pair.
- `SYNC_VALID`: at least eight samples and the latest beacon is no more than
  three beacon periods old.
- `SYNC_DEGRADED`: warmed model, age between three and ten periods.
- `SYNC_STALE`: age exceeds ten periods.

Tracking and raw CSI collection continue in every state. Only derived
cross-node alignment becomes unavailable outside VALID/DEGRADED. A new leader
session or follower boot session resets warmup; the immediately retired leader
session and duplicate/stale sequences are rejected. Sessions are not stored in
NVS. Node identity and placement/geometry identity remain separate concepts.

RuView informed the one-way beacon, smoothed offset, freshness and separate
telemetry approach. We did not copy its automatic leader election, reported
accuracy figures, phase-coherence implications, C6/802.15.4 assumptions, or
sequence/FPS interpolation. Published RuView material includes both strong lab
results and ESP32-S3 reports with much larger drift, so Newo exposes measured
distributions and defers accuracy claims to physical validation.

The NCSI `SYNC` record is the audit/replay source. Collector selection remains
independent: changing laptop/Termux destination never changes the leader clock.
Legacy archives without SYNC records decode normally and explicitly report
cross-node alignment unavailable.

Reviewed primary references:

- RuView witness log 110 (ESP-NOW beacon/EMA telemetry):
  <https://github.com/ruvnet/RuView/blob/main/docs/WITNESS-LOG-110.md>
- RuView ADR-110 (C6 extension and claimed timing context):
  <https://github.com/ruvnet/RuView/blob/main/docs/adr/ADR-110-esp32-c6-firmware-extension.md>
- RuView issue 1049 (contrary ESP32-S3 drift observations):
  <https://github.com/ruvnet/RuView/issues/1049>
- RuView ADR-031 (sensing-first RF assumptions):
  <https://github.com/ruvnet/RuView/blob/main/docs/adr/ADR-031-ruview-sensing-first-rf-mode.md>
