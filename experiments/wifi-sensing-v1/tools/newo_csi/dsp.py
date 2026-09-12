"""Conservative, streaming and geometry-partitioned CSI feature extraction."""

from __future__ import annotations

from collections import Counter, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
import hashlib
import json
import math
from statistics import median
from typing import Any

from .protocol import CsiRecord, PATH_NAMES, mac_text

FEATURE_SCHEMA_VERSION = 2
FEATURE_CONTRACT_NAME = "amplitude_derivative_power_v2"


def feature_contract(top_k: int, window_seconds: float) -> dict[str, Any]:
    return {
        "feature_schema_version": FEATURE_SCHEMA_VERSION,
        "feature_name": FEATURE_CONTRACT_NAME,
        "top_k": top_k,
        "window_seconds": window_seconds,
        "amplitude": {"transform": "magnitude", "normalization": "frozen_warmup_mean"},
        "phase": {"computed": True, "score_weight": 0.0,
                  "status": "diagnostic_only_pending_phase_sanitization_and_sync"},
        "derivative": {"time_scaled": True, "gap_policy": "skip_nonpositive_or_over_max_gap",
                       "max_gap_seconds": max(2.0, window_seconds)},
        "aggregation": {"subcarriers": "median_squared_derivative",
                        "time_window": "mean_power"},
    }


@dataclass
class Welford:
    count: int = 0
    mean: float = 0.0
    m2: float = 0.0

    def add(self, value: float) -> None:
        self.count += 1
        delta = value - self.mean
        self.mean += delta / self.count
        self.m2 += delta * (value - self.mean)

    @property
    def variance(self) -> float:
        return self.m2 / (self.count - 1) if self.count > 1 else 0.0

    @property
    def stddev(self) -> float:
        return math.sqrt(max(0.0, self.variance))


@dataclass(frozen=True)
class Geometry:
    path_id: int
    node_id: int
    receiver_mac: str
    source_mac: str
    channel: int
    secondary_channel: int
    bandwidth: int
    phy_mode: int
    ltf_mask: int
    payload_length: int
    subcarriers: int

    @classmethod
    def from_record(cls, record: CsiRecord) -> "Geometry":
        return cls(record.path_id, record.node_id, mac_text(record.receiver_mac),
                   mac_text(record.source_mac), record.channel,
                   record.secondary_channel, record.bandwidth, record.phy_mode,
                   record.ltf_mask, record.csi_payload_length,
                   record.subcarrier_item_count)

    @property
    def identity(self) -> str:
        return (f"p{self.path_id}:n{self.node_id}:{self.receiver_mac}>{self.source_mac}:"
                f"ch{self.channel}/{self.secondary_channel}:bw{self.bandwidth}:"
                f"phy{self.phy_mode}:ltf{self.ltf_mask}:len{self.payload_length}")


@dataclass
class PathSnapshot:
    path_id: int
    path_name: str
    geometry: str
    samples: int
    sample_rate_hz: float
    rssi_dbm: int | None
    signal_quality: str
    selected_subcarriers: int
    last_sequence: int | None
    sequence_gaps: int
    duplicates: int
    motion_score: float | None
    motion_state: str
    window_power: float | None
    calibrated: bool
    selected_indices: tuple[int, ...] = ()


class GeometryProcessor:
    def __init__(self, geometry: Geometry, top_k: int, window_seconds: float):
        self.geometry = geometry
        self.top_k = top_k
        self.window_ns = int(window_seconds * 1e9)
        n = geometry.subcarriers
        self.amplitude_stats = [Welford() for _ in range(n)]
        self.phase_stats = [Welford() for _ in range(n)]
        self.previous_phase: list[float] | None = None
        self.unwrapped_phase: list[float] | None = None
        self.previous_amplitude: list[float] | None = None
        self.previous_ns: int | None = None
        self.features: deque[tuple[int, float]] = deque()
        self.times: deque[int] = deque()
        self.rssi: deque[tuple[int, int]] = deque()
        self.noise: deque[tuple[int, int]] = deque()
        self.samples = 0
        self.last_sequence: int | None = None
        self.sequence_gaps = 0
        self.duplicates = 0
        self.baseline: dict[str, float] | None = None
        self.frozen_indices: tuple[int, ...] | None = None
        self.amplitude_scales: dict[int, float] = {}

    @staticmethod
    def iq(record: CsiRecord) -> tuple[list[float], list[float]]:
        amplitudes, phases = [], []
        for offset in range(0, len(record.iq_bytes), 2):
            imag = int.from_bytes(record.iq_bytes[offset:offset + 1], "little", signed=True)
            real = int.from_bytes(record.iq_bytes[offset + 1:offset + 2], "little", signed=True)
            amplitudes.append(math.hypot(real, imag))
            phases.append(math.atan2(imag, real))
        return amplitudes, phases

    def selected(self) -> tuple[int, ...]:
        if self.frozen_indices is not None:
            return self.frozen_indices
        usable = [i for i, stats in enumerate(self.amplitude_stats)
                  if stats.count >= 2 and stats.mean > 1e-9]
        usable.sort(key=lambda i: self.amplitude_stats[i].variance, reverse=True)
        return tuple(usable[:self.top_k])

    def freeze_selection(self, indices: tuple[int, ...] | None = None,
                         scales: dict[int, float] | None = None) -> None:
        chosen = self.selected() if indices is None else indices
        if not chosen or any(index < 0 or index >= self.geometry.subcarriers for index in chosen):
            raise ValueError("calibration selected subcarriers are invalid for geometry")
        self.frozen_indices = tuple(chosen)
        self.amplitude_scales = scales or {
            index: max(1.0, self.amplitude_stats[index].mean) for index in chosen
        }
        if set(self.amplitude_scales) != set(self.frozen_indices):
            raise ValueError("calibration normalization does not match selected subcarriers")
        self.features.clear()
        # Do not let the warm-up/baseline boundary create a derivative sample.
        self.previous_amplitude = None
        self.previous_phase = None
        self.unwrapped_phase = None
        self.previous_ns = None

    def add(self, record: CsiRecord, host_ns: int) -> None:
        amplitudes, phases = self.iq(record)
        if self.previous_phase is None:
            unwrapped = phases[:]
        else:
            unwrapped = []
            assert self.unwrapped_phase is not None
            for current, previous, old_unwrapped in zip(phases, self.previous_phase,
                                                         self.unwrapped_phase):
                delta = (current - previous + math.pi) % (2 * math.pi) - math.pi
                unwrapped.append(old_unwrapped + delta)

        for index, value in enumerate(amplitudes):
            self.amplitude_stats[index].add(value)
            self.phase_stats[index].add(unwrapped[index])

        indices = self.selected()
        delta_seconds = None if self.previous_ns is None else (host_ns - self.previous_ns) / 1e9
        if (self.previous_amplitude is not None and self.unwrapped_phase is not None and indices
                and delta_seconds is not None and 0 < delta_seconds <= max(2.0, self.window_ns / 1e9)):
            components = []
            for index in indices:
                scale = self.amplitude_scales.get(
                    index, max(1.0, self.amplitude_stats[index].mean))
                amp_delta = ((amplitudes[index] - self.previous_amplitude[index]) /
                             scale / delta_seconds)
                # Phase remains observable above, but is deliberately excluded from the
                # Phase-5 score until common-phase/CFO sanitization and sync are validated.
                components.append(amp_delta * amp_delta)
            self.features.append((host_ns, median(components)))

        self.previous_amplitude = amplitudes
        self.previous_phase = phases
        self.unwrapped_phase = unwrapped
        self.previous_ns = host_ns
        self.samples += 1
        self.last_sequence = record.sequence
        self.times.append(host_ns)
        self.rssi.append((host_ns, record.rssi_dbm))
        self.noise.append((host_ns, record.noise_floor_dbm))
        self.expire(host_ns)

    def expire(self, host_ns: int) -> None:
        cutoff = host_ns - self.window_ns
        while self.features and self.features[0][0] < cutoff:
            self.features.popleft()
        while self.times and self.times[0] < cutoff:
            self.times.popleft()
        while self.rssi and self.rssi[0][0] < cutoff:
            self.rssi.popleft()
        while self.noise and self.noise[0][0] < cutoff:
            self.noise.popleft()

    def power(self) -> float | None:
        return None if not self.features else sum(v for _, v in self.features) / len(self.features)

    def rate(self) -> float:
        if len(self.times) < 2:
            return 0.0
        span = (self.times[-1] - self.times[0]) / 1e9
        return 0.0 if span <= 0 else (len(self.times) - 1) / span

    def snapshot(self) -> PathSnapshot:
        power = self.power()
        score = None
        state = "LOW_CONFIDENCE"
        if self.baseline is not None and power is not None and len(self.features) >= 3:
            scale = max(self.baseline["stddev"], self.baseline["mean"] * 0.10, 1e-9)
            score = max(0.0, (power - self.baseline["mean"]) / (3.0 * scale))
            state = "QUIET" if score < 1.0 else ("RF_CHANGE" if score < 3.0 else "MOTION_CANDIDATE")
        rssi = None if not self.rssi else round(sum(v for _, v in self.rssi) / len(self.rssi))
        noise = None if not self.noise else sum(v for _, v in self.noise) / len(self.noise)
        snr = None if rssi is None or noise is None else rssi - noise
        rate = self.rate()
        quality = "LOW"
        if (rssi is not None and rssi >= -75 and snr is not None and snr >= 15
                and rate >= 5 and len(self.selected()) >= 4):
            quality = "GOOD"
        elif rssi is not None and rssi >= -85 and snr is not None and snr >= 8 and rate >= 1:
            quality = "FAIR"
        return PathSnapshot(self.geometry.path_id,
                            PATH_NAMES.get(self.geometry.path_id, f"PATH_{self.geometry.path_id}"),
                            self.geometry.identity, self.samples, rate, rssi, quality,
                            len(self.selected()), self.last_sequence, self.sequence_gaps, self.duplicates,
                            score, state, power, self.baseline is not None,
                            self.selected())


class CsiPipeline:
    """One processor per exact path/receiver/source/radio geometry."""

    def __init__(self, top_k: int = 24, window_seconds: float = 2.0):
        if top_k <= 0 or window_seconds <= 0:
            raise ValueError("top_k and window_seconds must be positive")
        self.top_k = top_k
        self.window_seconds = window_seconds
        self.processors: dict[Geometry, GeometryProcessor] = {}
        self.geometry_counts: Counter[Geometry] = Counter()
        self.calibration: dict[str, dict[str, float]] = {}
        self.calibration_metadata: dict[str, Any] = {}
        self.calibration_rejection: str | None = "calibration not loaded"
        self.calibration_stats: dict[str, Welford] | None = None
        self.calibration_stage: str | None = None
        self.calibration_frozen_geometries: set[str] = set()
        self._last_sequence: dict[tuple[int, str], int] = {}
        self._sequence_gaps: Counter[tuple[int, str]] = Counter()
        self._duplicates: Counter[tuple[int, str]] = Counter()

    def add(self, record: CsiRecord, host_ns: int) -> None:
        geometry = Geometry.from_record(record)
        stream = (record.node_id, geometry.receiver_mac)
        previous = self._last_sequence.get(stream)
        if record.sequence == 0 and record.csi_flags & (1 << 7):
            previous = None
        if previous is not None:
            delta = (record.sequence - previous) & 0xFFFFFFFF
            if delta == 0:
                self._duplicates[stream] += 1
            elif delta < 0x80000000:
                self._sequence_gaps[stream] += max(0, delta - 1)
        if previous is None or ((record.sequence - previous) & 0xFFFFFFFF) < 0x80000000:
            self._last_sequence[stream] = record.sequence
        processor = self.processors.get(geometry)
        if processor is None:
            processor = self.processors[geometry] = GeometryProcessor(
                geometry, self.top_k, self.window_seconds)
            processor.baseline = self.calibration.get(geometry.identity)
            if processor.baseline is not None:
                try:
                    processor.freeze_selection(
                        tuple(processor.baseline["selected_subcarriers"]),
                        {int(key): value for key, value in
                         processor.baseline["amplitude_scales"].items()})
                except (KeyError, TypeError, ValueError):
                    processor.baseline = None
                    self.calibration = {}
                    self.calibration_rejection = "calibration path feature data invalid"
        self.geometry_counts[geometry] += 1
        processor.add(record, host_ns)
        if (self.calibration_stage == "baseline" and self.calibration_stats is not None
                and geometry.identity in self.calibration_frozen_geometries):
            power = processor.power()
            if power is not None:
                self.calibration_stats.setdefault(geometry.identity, Welford()).add(power)

    def receiver_losses(self) -> dict[tuple[int, str], dict[str, int]]:
        streams = set(self._last_sequence) | set(self._sequence_gaps) | set(self._duplicates)
        return {stream: {"gaps": self._sequence_gaps[stream],
                         "duplicates": self._duplicates[stream]}
                for stream in sorted(streams)}

    def dominant(self, path_id: int) -> GeometryProcessor | None:
        choices = [p for g, p in self.processors.items() if g.path_id == path_id]
        # Runtime dominance follows the active analysis window; cumulative counts
        # remain the deterministic tie-break and long-run audit evidence.
        return max(choices, key=lambda p: (len(p.times), self.geometry_counts[p.geometry]),
                   default=None)

    def expire(self, host_ns: int) -> None:
        """Advance window state without adding or changing any feature sample."""
        for processor in self.processors.values():
            processor.expire(host_ns)

    def snapshots(self) -> dict[int, PathSnapshot]:
        result = {}
        for path in (1, 2, 3):
            processor = self.dominant(path)
            if processor is None:
                continue
            snapshot = processor.snapshot()
            stream = (processor.geometry.node_id, processor.geometry.receiver_mac)
            snapshot.sequence_gaps = self._sequence_gaps[stream]
            snapshot.duplicates = self._duplicates[stream]
            result[path] = snapshot
        return result

    def calibration_document(self, placement: str | None,
                             room_id: str | None = None) -> dict[str, Any]:
        paths: dict[str, Any] = {}
        source = self.calibration_stats or {}
        for geometry, processor in self.processors.items():
            if geometry.identity not in self.calibration_frozen_geometries:
                continue
            stats = source.get(geometry.identity, Welford())
            if stats.count >= 3:
                paths[geometry.identity] = {
                    "path_id": geometry.path_id, "node_id": geometry.node_id,
                    "receiver_mac": geometry.receiver_mac, "source_mac": geometry.source_mac,
                    "geometry": geometry.identity, "samples": stats.count,
                    "mean": stats.mean, "variance": stats.variance,
                    "stddev": stats.stddev,
                    "selected_subcarriers": list(processor.selected()),
                    "amplitude_scales": {str(index): processor.amplitude_scales[index]
                                         for index in processor.selected()},
                }
        document = {"schema_version": 3, "room_id": room_id,
                "placement": placement, "feature_contract": feature_contract(
                    self.top_k, self.window_seconds),
                "created_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
                "calibration_policy": {"selection_fraction": 0.4,
                                       "selection_stage": "amplitude variance ranking",
                                       "baseline_stage": "frozen selection power statistics"},
                "paths": paths}
        identity_payload = json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8")
        document["calibration_id"] = hashlib.sha256(identity_payload).hexdigest()[:16]
        return document

    def begin_calibration(self) -> None:
        for processor in self.processors.values():
            processor.amplitude_stats = [Welford() for _ in range(processor.geometry.subcarriers)]
            processor.phase_stats = [Welford() for _ in range(processor.geometry.subcarriers)]
            processor.baseline = None
            processor.frozen_indices = None
            processor.amplitude_scales = {}
            processor.features.clear()
            processor.previous_amplitude = None
            processor.previous_phase = None
            processor.unwrapped_phase = None
            processor.previous_ns = None
        self.calibration_stats = {}
        self.calibration_stage = "selection"
        self.calibration_frozen_geometries = set()

    def freeze_calibration_selection(self) -> None:
        if self.calibration_stage != "selection":
            raise ValueError("calibration selection stage is not active")
        for processor in self.processors.values():
            if processor.selected():
                processor.freeze_selection()
                self.calibration_frozen_geometries.add(processor.geometry.identity)
        self.calibration_stats = {}
        self.calibration_stage = "baseline"

    def load_calibration(self, document: dict[str, Any], placement: str | None,
                         room_id: str | None = None) -> None:
        self.calibration = {}
        for processor in self.processors.values():
            processor.baseline = None
            processor.frozen_indices = None
            processor.amplitude_scales = {}
        self.calibration_metadata = {
            "schema_version": document.get("schema_version"),
            "created_at": document.get("created_at"),
            "room_id": document.get("room_id"),
            "placement": document.get("placement"),
            "feature_contract": document.get("feature_contract"),
        }
        if document.get("schema_version", 0) < 3 or "feature_contract" not in document:
            self.calibration_rejection = "legacy calibration incompatible: frozen feature contract missing"
            return
        incoming = document["feature_contract"]
        expected = feature_contract(self.top_k, self.window_seconds)
        if incoming.get("feature_schema_version") != FEATURE_SCHEMA_VERSION:
            self.calibration_rejection = "feature schema mismatch"
            return
        for key in ("top_k", "window_seconds", "amplitude", "phase", "derivative",
                    "aggregation", "feature_name"):
            if incoming.get(key) != expected.get(key):
                self.calibration_rejection = f"dsp config mismatch: {key}"
                return
        if placement is None:
            self.calibration_rejection = "placement missing"
            return
        if document.get("placement") != placement:
            self.calibration_rejection = "placement mismatch"
            return
        calibration_room = document.get("room_id")
        if calibration_room is not None and room_id is None:
            self.calibration_rejection = "room missing"
            return
        if calibration_room is not None and calibration_room != room_id:
            self.calibration_rejection = "room mismatch"
            return
        paths = document.get("paths", {})
        if not isinstance(paths, dict):
            self.calibration_rejection = "calibration paths invalid"
            return
        if any("selected_subcarriers" not in value or "amplitude_scales" not in value
               for value in paths.values() if isinstance(value, dict)) or any(
                   not isinstance(value, dict) for value in paths.values()):
            self.calibration_rejection = "legacy calibration incompatible: frozen subcarriers missing"
            return
        try:
            for identity, value in paths.items():
                if value.get("geometry", identity) != identity:
                    raise ValueError("geometry identity mismatch")
                selected = value["selected_subcarriers"]
                scales = value["amplitude_scales"]
                if (not isinstance(selected, list) or not selected
                        or any(not isinstance(index, int) or index < 0 for index in selected)
                        or len(set(selected)) != len(selected) or not isinstance(scales, dict)
                        or {str(index) for index in selected} != set(scales)):
                    raise ValueError("selected subcarriers or scales invalid")
                numeric = (value.get("mean"), value.get("variance"), value.get("stddev"))
                if any(not isinstance(item, (int, float)) or not math.isfinite(item) or item < 0
                       for item in numeric):
                    raise ValueError("baseline statistics invalid")
                if any(not isinstance(scales[str(index)], (int, float))
                       or not math.isfinite(scales[str(index)]) or scales[str(index)] <= 0
                       for index in selected):
                    raise ValueError("amplitude scales invalid")
        except (KeyError, TypeError, ValueError):
            self.calibration_rejection = "calibration path feature data invalid"
            return
        self.calibration = {key: value for key, value in paths.items()}
        self.calibration_rejection = None if self.calibration else "calibration contains no usable paths"
        for geometry, processor in self.processors.items():
            processor.baseline = self.calibration.get(geometry.identity)
            if processor.baseline is not None:
                try:
                    processor.freeze_selection(
                        tuple(processor.baseline["selected_subcarriers"]),
                        {int(key): value for key, value in
                         processor.baseline["amplitude_scales"].items()})
                except (KeyError, TypeError, ValueError):
                    processor.baseline = None
                    self.calibration = {}
                    self.calibration_rejection = "calibration path feature data invalid"

    def calibration_status(self) -> str:
        return self.calibration_report()["status"]

    def calibration_report(self, now: datetime | None = None) -> dict[str, Any]:
        created = self.calibration_metadata.get("created_at")
        age_seconds = None
        if created:
            try:
                parsed = datetime.fromisoformat(str(created).replace("Z", "+00:00"))
                age_seconds = max(0.0, ((now or datetime.now(timezone.utc)) - parsed).total_seconds())
            except (TypeError, ValueError):
                pass
        base = {**self.calibration_metadata, "age_seconds": age_seconds}
        if self.calibration_rejection:
            return {**base, "status": "REJECTED", "reason": self.calibration_rejection,
                    "matched_paths": [], "mismatched_geometries": []}
        current = list(self.processors.values())
        if not current:
            return {**base, "status": "PENDING", "reason": "awaiting CSI geometry",
                    "matched_paths": [], "mismatched_geometries": []}
        matched = [p.geometry.identity for p in current if p.geometry.identity in self.calibration]
        mismatched = [p.geometry.identity for p in current if p.geometry.identity not in self.calibration]
        dominant = {path: self.dominant(path) for path in (1, 2, 3)}
        unmatched_dominant_paths = [path for path, processor in dominant.items()
                                    if processor is not None
                                    and processor.geometry.identity not in self.calibration]
        if mismatched:
            return {**base, "status": "PARTIAL", "reason": "unmatched observed geometries",
                    "matched_paths": matched, "mismatched_geometries": mismatched,
                    "unmatched_dominant_paths": unmatched_dominant_paths}
        return {**base, "status": "MATCH", "reason": None,
                "matched_paths": matched, "mismatched_geometries": [],
                "unmatched_dominant_paths": []}

    def fused(self, suspended: bool = False) -> tuple[str, float]:
        if suspended:
            return "REPOSITIONING", 0.0
        snapshots = [s for s in self.snapshots().values()
                     if s.calibrated and s.signal_quality != "LOW" and s.motion_score is not None]
        if not snapshots:
            return "LOW_CONFIDENCE", 0.0
        candidates = sum(s.motion_state == "MOTION_CANDIDATE" for s in snapshots)
        changes = sum(s.motion_state in ("RF_CHANGE", "MOTION_CANDIDATE") for s in snapshots)
        confidence = min(1.0, len(snapshots) / 3.0) * (0.5 + 0.5 * max(candidates, changes) / len(snapshots))
        if candidates >= 2:
            return "MOTION_CANDIDATE", confidence
        if changes >= 2:
            return "RF_CHANGE", confidence
        if changes == 1:
            return "LOW_CONFIDENCE", confidence * 0.5
        return "QUIET", confidence
