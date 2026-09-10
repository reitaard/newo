"""Objective offline reports produced by the shared Phase-5 DSP pipeline."""

from __future__ import annotations

from collections import Counter, defaultdict
from datetime import datetime, timezone
import itertools
import json
import math
from pathlib import Path
from typing import Any, Iterable

from .archive import ArchivedRecord, iter_archive
from .annotations import DEFAULT_ANNOTATIONS_DIR, load_annotations
from .dsp import CsiPipeline, Geometry
from .protocol import CsiRecord, PATH_NAMES, decode
from .statistics import CaptureStats

STATES = ("QUIET", "RF_CHANGE", "MOTION_CANDIDATE", "LOW_CONFIDENCE", "REPOSITIONING")
ACTIVE_STATES = ("RF_CHANGE", "MOTION_CANDIDATE")


def percentile(values: Iterable[float], q: float) -> float | None:
    data = sorted(values)
    if not data:
        return None
    if not 0 <= q <= 1:
        raise ValueError("percentile must be between zero and one")
    position = (len(data) - 1) * q
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return data[lower]
    fraction = position - lower
    return data[lower] * (1 - fraction) + data[upper] * fraction


def longest_run(windows: list[dict[str, Any]], path_name: str, state: str,
                window_seconds: float) -> float:
    longest = current = 0
    for window in windows:
        if window["paths"].get(path_name, {}).get("state") == state:
            current += 1
            longest = max(longest, current)
        else:
            current = 0
    return longest * window_seconds


def agreement_runs(windows: list[dict[str, Any]], count: int,
                   window_seconds: float) -> dict[str, float | int]:
    hits = [sum(value.get("state") in ACTIVE_STATES for value in row["paths"].values()) == count
            for row in windows]
    runs = 0
    previous = False
    for hit in hits:
        if hit and not previous:
            runs += 1
        previous = hit
    return {"count": runs, "duration_seconds": round(sum(hits) * window_seconds, 6)}


def single_path_patterns(windows: list[dict[str, Any]], window_seconds: float) -> tuple[dict[str, Any], dict[str, Any]]:
    hits = [sum(value.get("state") in ACTIVE_STATES for value in row["paths"].values()) == 1
            for row in windows]
    lengths: list[int] = []
    current = 0
    for hit in hits + [False]:
        if hit:
            current += 1
        elif current:
            lengths.append(current)
            current = 0
    transient = [length for length in lengths if length == 1]
    sustained = [length for length in lengths if length > 1]
    describe = lambda values: {"count": len(values),
                               "duration_seconds": round(sum(values) * window_seconds, 6)}
    return describe(transient), describe(sustained)


def _load_labels(session: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    metadata_path = session / "session.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8")) if metadata_path.is_file() else {}
    events = []
    events_path = session / "events.jsonl"
    if events_path.is_file():
        for line in events_path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                events.append(json.loads(line))
    return metadata, events


def evaluate_archive(session_value: str | Path, window_seconds: float,
                     calibration: dict[str, Any] | None = None,
                     annotations_dir: Path = DEFAULT_ANNOTATIONS_DIR) -> dict[str, Any]:
    session = Path(session_value)
    archive_path = session / "frames.ncsi" if session.is_dir() else session
    metadata, events = _load_labels(session) if session.is_dir() else ({}, [])
    reposition_ranges = []
    started = None
    for event in events:
        if event.get("event") == "repositioning_started":
            started = event.get("host_monotonic_ns")
        elif event.get("event") == "repositioning_ended" and started is not None:
            reposition_ranges.append((started, event.get("host_monotonic_ns", started)))
            started = None
    if started is not None:
        reposition_ranges.append((started, (1 << 63) - 1))
    inference = evaluate_records(iter_archive(archive_path), window_seconds, calibration,
                              metadata.get("placement_label"), metadata.get("room_id"),
                              reposition_ranges)
    session_id = metadata.get("session_id", session.name)
    annotations = load_annotations(session_id, annotations_dir) if session.is_dir() else []
    return {
        "schema_version": 1, "dataset": str(session),
        "original_capture_metadata": metadata,
        "original_capture_events": events,
        "operator_post_hoc_annotations": annotations,
        "dsp_inference": inference,
        "separation_boundary": "capture metadata, post-hoc operator annotations, and DSP inference are independent evidence",
    }


def evaluate_records(items: Iterable[ArchivedRecord], window_seconds: float,
                     calibration: dict[str, Any] | None = None,
                     placement: str | None = None,
                     room_id: str | None = None,
                     reposition_ranges: list[tuple[int, int]] | None = None) -> dict[str, Any]:
    if window_seconds <= 0:
        raise ValueError("window_seconds must be positive")
    pipeline = CsiPipeline(window_seconds=window_seconds)
    if calibration:
        pipeline.load_calibration(calibration, placement, room_id)
    capture = CaptureStats()
    iterator = iter(items)
    first_item = next(iterator, None)
    if first_item is None:
        return _empty_result(window_seconds)
    first_ns = first_item.host_monotonic_ns
    first_wall = first_item.host_wall_ns
    last_ns = first_ns
    path_first: dict[int, int] = {}
    path_last: dict[int, int] = {}
    path_samples: Counter[int] = Counter()
    geometries: dict[int, list[str]] = defaultdict(list)
    previous_geometry: dict[int, str] = {}
    geometry_transitions: Counter[int] = Counter()
    windows: list[dict[str, Any]] = []
    window_ns = int(window_seconds * 1e9)
    reposition_ranges = reposition_ranges or []
    window_start = first_ns
    boundary = window_start + window_ns
    window_has_records = False

    def emit(end_ns: int, complete: bool) -> None:
        pipeline.expire(end_ns)
        snapshots = pipeline.snapshots()
        paths = {}
        for path_id in (1, 2, 3):
            snapshot = snapshots.get(path_id)
            name = PATH_NAMES[path_id]
            if snapshot is None or snapshot.sample_rate_hz <= 0:
                paths[name] = {"score": None, "state": "LOW_CONFIDENCE",
                               "signal_quality": "LOW", "sample_rate_hz": 0.0}
            else:
                paths[name] = {
                    "score": snapshot.motion_score, "state": snapshot.motion_state,
                    "signal_quality": snapshot.signal_quality,
                    "sample_rate_hz": snapshot.sample_rate_hz,
                    "rssi_dbm": snapshot.rssi_dbm,
                    "selected_subcarriers": snapshot.selected_subcarriers,
                }
        fused_state, confidence = pipeline.fused(False)
        if any(start <= end_ns <= end for start, end in reposition_ranges):
            for value in paths.values():
                value["score"] = None
                value["state"] = "REPOSITIONING"
            fused_state, confidence = "REPOSITIONING", 0.0
        elapsed = (end_ns - first_ns) / 1e9
        wall_ns = first_wall + (end_ns - first_ns)
        windows.append({"index": len(windows), "elapsed_seconds": round(elapsed, 6),
                        "timestamp": datetime.fromtimestamp(wall_ns / 1e9, timezone.utc).isoformat(),
                        "window_duration_seconds": round((end_ns - (first_ns + len(windows) * window_ns)) / 1e9, 6),
                        "partial": not complete, "included_in_aggregate": complete,
                        "paths": paths, "fused_state": fused_state,
                        "fusion_confidence": confidence})

    for item in itertools.chain((first_item,), iterator):
        last_ns = item.host_monotonic_ns
        while item.host_monotonic_ns >= boundary:
            emit(boundary, True)
            boundary += window_ns
            window_has_records = False
        record = decode(item.record)
        capture.add(record, item.host_monotonic_ns)
        window_has_records = True
        if not isinstance(record, CsiRecord):
            continue
        path_first.setdefault(record.path_id, item.host_monotonic_ns)
        path_last[record.path_id] = item.host_monotonic_ns
        path_samples[record.path_id] += 1
        identity = Geometry.from_record(record).identity
        if identity != previous_geometry.get(record.path_id):
            if record.path_id in previous_geometry:
                geometry_transitions[record.path_id] += 1
            previous_geometry[record.path_id] = identity
            if identity not in geometries[record.path_id]:
                geometries[record.path_id].append(identity)
        pipeline.add(record, item.host_monotonic_ns)
    partial_start = first_ns + len(windows) * window_ns
    if window_has_records:
        emit(last_ns, False)

    path_results = {}
    for path_id in (1, 2, 3):
        name = PATH_NAMES[path_id]
        aggregate_windows = [row for row in windows if row["included_in_aggregate"]]
        path_windows = [row["paths"][name] for row in aggregate_windows]
        scores = [row["score"] for row in path_windows if row.get("score") is not None]
        quality = Counter(row["signal_quality"] for row in path_windows)
        states = Counter(row["state"] for row in path_windows)
        valid = sum(states[state] for state in STATES)
        duration = 0.0
        if path_id in path_first:
            duration = max(0.0, (path_last[path_id] - path_first[path_id]) / 1e9)
        sample_count = path_samples[path_id]
        path_results[name] = {
            "usable_duration_seconds": round(duration, 6),
            "sample_count": sample_count,
            "effective_sample_rate_hz": 0.0 if duration <= 0 else round((sample_count - 1) / duration, 6),
            "signal_quality_distribution": dict(sorted(quality.items())),
            "motion_score": {key: value for key, value in (
                ("min", min(scores) if scores else None), ("median", percentile(scores, .5)),
                ("p90", percentile(scores, .9)), ("p95", percentile(scores, .95)),
                ("p99", percentile(scores, .99)), ("max", max(scores) if scores else None))},
            "state_fraction": {state: (states[state] / valid if valid else 0.0) for state in STATES},
            "longest_rf_change_seconds": longest_run(aggregate_windows, name, "RF_CHANGE", window_seconds),
            "longest_motion_candidate_seconds": longest_run(aggregate_windows, name, "MOTION_CANDIDATE", window_seconds),
            "geometry_transition_count": geometry_transitions[path_id],
            "geometries": geometries[path_id],
        }
    summary = capture.as_dict()
    aggregate_windows = [row for row in windows if row["included_in_aggregate"]]
    transient, sustained = single_path_patterns(aggregate_windows, window_seconds)
    degraded = [any(value.get("signal_quality") == "LOW" for value in row["paths"].values())
                for row in aggregate_windows]
    return {
        "schema_version": 1, "window_seconds": window_seconds,
        "duration_seconds": round(max(0, last_ns - first_ns) / 1e9, 6),
        "paths": path_results, "windows": windows,
        "nuisance_patterns": {
            "isolated_single_path": transient,
            "single_path_transient": transient,
            "single_path_sustained": sustained,
            "two_path_agreement": agreement_runs(aggregate_windows, 2, window_seconds),
            "three_path_agreement": agreement_runs(aggregate_windows, 3, window_seconds),
            "receiver_network_degradation": {
                "window_count": sum(degraded),
                "duration_seconds": round(sum(degraded) * window_seconds, 6),
            },
        },
        "transport": {
            "record_counts": summary["record_counts"],
            "sequence_gap_estimate_by_receiver": summary["sequence_gap_estimate_by_receiver"],
            "duplicates_by_receiver": summary["duplicates_by_receiver"],
            "latest_device_drop_counters": summary["latest_device_drop_counters"],
        },
        "calibration": pipeline.calibration_report(),
        "partial_window_count": sum(row["partial"] for row in windows),
        "interpretation_boundary": "RF path change/agreement only; no person, identity, location, or nuisance-class inference",
    }


def _empty_result(window_seconds: float) -> dict[str, Any]:
    return {"schema_version": 1, "window_seconds": window_seconds, "duration_seconds": 0.0,
            "paths": {}, "windows": [],
            "nuisance_patterns": {name: {"count": 0, "duration_seconds": 0.0} for name in
                                  ("isolated_single_path", "single_path_transient",
                                   "single_path_sustained", "two_path_agreement",
                                   "three_path_agreement")},
            "transport": {"record_counts": {}, "sequence_gap_estimate_by_receiver": {},
                          "duplicates_by_receiver": {}, "latest_device_drop_counters": []},
            "calibration": {"status": "UNAVAILABLE", "reason": "empty archive",
                            "metadata": {}, "age_seconds": None,
                            "matched_paths": [], "mismatched_geometries": []},
            "partial_window_count": 0,
            "interpretation_boundary": "empty archive; no inference"}


def human_report(result: dict[str, Any]) -> str:
    inference = result["dsp_inference"]
    lines = [f"DATASET {result.get('dataset', '-')}", "ORIGINAL CAPTURE METADATA",
             json.dumps(result["original_capture_metadata"], sort_keys=True),
             "OPERATOR POST-HOC ANNOTATION",
             json.dumps(result["operator_post_hoc_annotations"], sort_keys=True),
             "DSP INFERENCE",
             f"duration={inference['duration_seconds']:.3f}s window={inference['window_seconds']:.3f}s"]
    calibration = inference.get("calibration", {})
    lines.append(f"calibration={calibration.get('status', 'UNAVAILABLE')} "
                 f"reason={calibration.get('reason', '-')} age_seconds={calibration.get('age_seconds')}")
    for name, path in inference["paths"].items():
        score = path["motion_score"]
        lines.append(f"{name}: samples={path['sample_count']} rate={path['effective_sample_rate_hz']:.2f}Hz "
                     f"quality={path['signal_quality_distribution']} score_med/p95/max="
                     f"{score['median']}/{score['p95']}/{score['max']} geometry_changes={path['geometry_transition_count']}")
        lines.append(f"  states={path['state_fraction']} longest_change={path['longest_rf_change_seconds']:.1f}s "
                     f"longest_candidate={path['longest_motion_candidate_seconds']:.1f}s")
    lines.append(f"agreement={inference['nuisance_patterns']}")
    lines.append("NOTE: saved labels are separate operator annotations, not proof of inferred activity.")
    return "\n".join(lines)


def human_comparison(results: list[dict[str, Any]]) -> str:
    lines = ["DATASET | PATH | Hz | p95 | candidate% | seq gaps | geometry changes"]
    for result in results:
        inference = result["dsp_inference"]
        dataset = Path(result.get("dataset", "-")).name
        gaps = sum(inference["transport"]["sequence_gap_estimate_by_receiver"].values())
        for name, path in inference["paths"].items():
            candidate = 100 * path["state_fraction"]["MOTION_CANDIDATE"]
            lines.append(f"{dataset} | {name} | {path['effective_sample_rate_hz']:.2f} | "
                         f"{path['motion_score']['p95']} | {candidate:.1f} | {gaps} | "
                         f"{path['geometry_transition_count']}")
    lines.append("Labels and differences are descriptive evidence, not proof of detected people or causes.")
    return "\n".join(lines)
