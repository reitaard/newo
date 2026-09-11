"""Derived Phase-6 clock alignment; raw device timestamps remain immutable."""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from math import ceil, floor
from typing import Any

from .protocol import CsiRecord, SyncRecord

SYNC_STATES = {0: "UNSYNCED", 1: "SYNC_WARMING", 2: "SYNC_VALID",
               3: "SYNC_DEGRADED", 4: "SYNC_STALE"}


def percentile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    values = sorted(values); position = (len(values) - 1) * q
    lo, hi = floor(position), ceil(position)
    return values[lo] if lo == hi else values[lo] * (hi-position) + values[hi] * (position-lo)


@dataclass
class ClockModel:
    record: SyncRecord
    host_monotonic_ns: int

    def align(self, local_us: int) -> float | None:
        if self.record.sync_state not in (2, 3):
            return None
        drift_scale = self.record.drift_milli_ppm / 1_000_000_000.0
        delta = local_us - self.record.local_timestamp_us
        return local_us + self.record.smoothed_offset_us + delta * drift_scale


class SyncAnalyzer:
    def __init__(self) -> None:
        self.models: dict[int, ClockModel] = {}
        self.records: list[tuple[int, SyncRecord]] = []
        self.rejected = 0
        self.retired_sessions: dict[int, set[tuple[int, int]]] = {}

    def add(self, record: SyncRecord, host_monotonic_ns: int) -> bool:
        if record.sync_version != 1:
            self.rejected += 1
            return False
        previous = self.models.get(record.node_id)
        epoch = (record.leader_session_id, record.follower_session_id)
        if epoch in self.retired_sessions.get(record.node_id, set()):
            self.rejected += 1
            return False
        if previous:
            previous_epoch = (previous.record.leader_session_id,
                              previous.record.follower_session_id)
            if epoch == previous_epoch and record.sync_sequence <= previous.record.sync_sequence:
                self.rejected += 1
                return False
            if epoch != previous_epoch:
                self.retired_sessions.setdefault(record.node_id, set()).add(previous_epoch)
        self.models[record.node_id] = ClockModel(record, host_monotonic_ns)
        self.records.append((host_monotonic_ns, record))
        return True

    def align(self, record: CsiRecord) -> float | None:
        model = self.models.get(record.node_id)
        return model.align(record.timestamp_us) if model else None

    def evidence(self, record: CsiRecord) -> dict[str, Any]:
        if record.node_id == 1:
            return {"node_id": 1, "raw_local_timestamp_us": record.timestamp_us,
                    "aligned_newo_timestamp_us": float(record.timestamp_us),
                    "sync_state": "LEADER_REFERENCE"}
        aligned = self.align(record)
        model = self.models.get(record.node_id)
        return {"node_id": record.node_id, "raw_local_timestamp_us": record.timestamp_us,
                "aligned_newo_timestamp_us": aligned,
                "sync_state": (SYNC_STATES.get(model.record.sync_state, "UNKNOWN")
                               if model else "UNAVAILABLE")}

    def status(self, node_id: int = 2) -> dict[str, Any]:
        model = self.models.get(node_id)
        if model is None:
            return {"state": "UNAVAILABLE", "cross_node_alignment_available": False}
        record = model.record
        state = SYNC_STATES.get(record.sync_state, "UNKNOWN")
        return {"state": state,
                "cross_node_alignment_available": record.sync_state in (2, 3),
                "leader_session_id": record.leader_session_id,
                "follower_session_id": record.follower_session_id,
                "last_sync_age_us": record.last_sync_age_us}

    def summary(self, duration_seconds: float = 0.0,
                capture_start_ns: int | None = None,
                capture_end_ns: int | None = None) -> dict[str, Any]:
        if not self.records:
            return {"state": "UNAVAILABLE", "sample_count": 0, "rejected_samples": self.rejected,
                    "interpretation": "no SYNC records; cross-node temporal fusion unavailable"}
        raw = [float(r.raw_offset_us) for _, r in self.records]
        smooth = [float(r.smoothed_offset_us) for _, r in self.records]
        residual = [abs(a-b) for a, b in zip(raw, smooth)]
        states = Counter(SYNC_STATES.get(r.sync_state, "UNKNOWN") for _, r in self.records)
        total = len(self.records)
        latest = self.records[-1][1]
        epochs = sorted({(r.leader_session_id, r.follower_session_id)
                         for _, r in self.records})
        state_seconds: Counter[str] = Counter()
        if capture_start_ns is not None:
            state_seconds["UNSYNCED"] += max(
                0.0, (self.records[0][0] - capture_start_ns) / 1e9)
        longest_stale = 0.0
        stale_run = 0.0
        for index, (host_ns, record) in enumerate(self.records):
            next_ns = (self.records[index + 1][0] if index + 1 < total
                       else (capture_end_ns if capture_end_ns is not None else host_ns))
            span = max(0.0, (next_ns - host_ns) / 1e9)
            name = SYNC_STATES.get(record.sync_state, "UNKNOWN")
            state_seconds[name] += span
            if name == "SYNC_STALE":
                stale_run += span
                longest_stale = max(longest_stale, stale_run)
            else:
                stale_run = 0.0
        covered = sum(state_seconds.values())
        known_states = tuple(SYNC_STATES.values())
        fractions = ({name: state_seconds[name] / covered for name in known_states}
                     if covered > 0 else
                     {name: states[name] / total for name in known_states})
        return {"state": SYNC_STATES.get(latest.sync_state, "UNKNOWN"),
                "sample_count": total, "accepted_samples": latest.accepted_samples,
                "rejected_samples": latest.rejected_samples + self.rejected,
                "initial_offset_us": raw[0], "final_offset_us": raw[-1],
                "raw_offset_us": {"median": percentile(raw, .5), "p95": percentile(raw, .95), "p99": percentile(raw, .99)},
                "smoothed_offset_us": {"median": percentile(smooth, .5), "p95": percentile(smooth, .95), "p99": percentile(smooth, .99)},
                "residual_us": {"median": percentile(residual, .5), "p95": percentile(residual, .95), "p99": percentile(residual, .99)},
                "drift_ppm": None if latest.accepted_samples < 8 else latest.drift_milli_ppm / 1000.0,
                "last_sync_age_us": latest.last_sync_age_us,
                "jitter_us": latest.jitter_us,
                "state_fraction": fractions,
                "longest_stale_interval_seconds": longest_stale,
                "leader_session_id": latest.leader_session_id,
                "follower_session_id": latest.follower_session_id,
                "session_epoch_count": len(epochs),
                "session_epochs": [{"leader_session_id": leader,
                                    "follower_session_id": follower}
                                   for leader, follower in epochs],
                "interpretation": "one-way event-time alignment only; not RF phase coherence"}
