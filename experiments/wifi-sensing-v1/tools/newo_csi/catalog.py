"""Read-only inventory for immutable NCSI research sessions."""

from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
from typing import Any

from .annotations import DEFAULT_ANNOTATIONS_DIR, load_annotations
from .archive import iter_archive
from .dsp import Geometry
from .protocol import CsiRecord, PATH_NAMES, decode
from .statistics import CaptureStats


def calibration_compatibility(metadata: dict[str, Any], geometries: set[str],
                              calibration: dict[str, Any] | None) -> dict[str, Any]:
    if calibration is None:
        return {"status": "NOT_PROVIDED", "reason": "calibration not supplied"}
    placement = metadata.get("placement_label")
    if placement is None:
        return {"status": "REJECTED", "reason": "placement missing"}
    if calibration.get("placement") != placement:
        return {"status": "REJECTED", "reason": "placement mismatch"}
    calibration_room = calibration.get("room_id")
    if calibration_room is not None and metadata.get("room_id") is None:
        return {"status": "REJECTED", "reason": "room missing"}
    if calibration_room is not None and calibration_room != metadata.get("room_id"):
        return {"status": "REJECTED", "reason": "room mismatch"}
    missing = sorted(geometry for geometry in geometries
                     if geometry not in calibration.get("paths", {}))
    if missing:
        return {"status": "REJECTED", "reason": "geometry mismatch",
                "mismatched_geometries": missing}
    if not geometries:
        return {"status": "PENDING", "reason": "no CSI geometry"}
    return {"status": "MATCH", "reason": None}


def inspect_session(session: Path, calibration: dict[str, Any] | None,
                    annotations_dir: Path = DEFAULT_ANNOTATIONS_DIR) -> dict[str, Any]:
    metadata: dict[str, Any] = {}
    stats = CaptureStats()
    geometries: set[str] = set()
    error = None
    try:
        metadata = json.loads((session / "session.json").read_text(encoding="utf-8"))
        for item in iter_archive(session / "frames.ncsi"):
            record = decode(item.record)
            stats.add(record, item.host_monotonic_ns)
            if isinstance(record, CsiRecord):
                geometries.add(Geometry.from_record(record).identity)
    except (OSError, ValueError, json.JSONDecodeError) as caught:
        error = str(caught)
    summary = stats.as_dict()
    duration = summary["duration_seconds"]
    started = metadata.get("capture_started_monotonic_ns")
    ended = metadata.get("capture_ended_monotonic_ns")
    if isinstance(started, int) and isinstance(ended, int) and ended >= started:
        duration = (ended - started) / 1e9
    session_id = metadata.get("session_id", session.name)
    present = sorted({name for name in summary["paths"]})
    gaps = sum(summary["sequence_gap_estimate_by_receiver"].values())
    device_drops = sum(entry.get("session_delta_estimate", {}).get("device_transport_drops", 0)
                       for entry in summary["latest_device_drop_counters"])
    created = metadata.get("capture_started_at")
    return {
        "session_id": session_id, "capture_timestamp": created,
        "duration_seconds": round(duration, 6), "room_id": metadata.get("room_id"),
        "placement_label": metadata.get("placement_label"),
        "predates_placement_metadata": "placement_label" not in metadata,
        "original_scenario": metadata.get("scenario_label"),
        "original_person": metadata.get("person_label"),
        "original_activity": metadata.get("activity_label"),
        "operator_post_hoc_annotations": load_annotations(session_id, annotations_dir),
        "frame_count": summary["record_counts"]["csi"],
        "paths_present": present,
        "paths_missing": [PATH_NAMES[path] for path in (1, 2, 3)
                          if PATH_NAMES[path] not in present],
        "receiver_sequence_gaps": gaps, "device_transport_drops": device_drops,
        "transport_quality": "GOOD" if gaps == 0 and device_drops == 0 else "LOSS_OBSERVED",
        "calibration": calibration_compatibility(metadata, geometries, calibration),
        "raw_archive": {"status": "READABLE" if error is None else "ERROR", "error": error},
    }


def build_catalog(root: Path, calibration: dict[str, Any] | None,
                  annotations_dir: Path = DEFAULT_ANNOTATIONS_DIR) -> dict[str, Any]:
    sessions = []
    if root.is_dir() and (root / "session.json").is_file():
        candidates = [root]
    elif root.is_dir():
        candidates = sorted(path for path in root.iterdir() if path.is_dir())
    else:
        raise ValueError(f"catalog root not found: {root}")
    for session in candidates:
        if (session / "session.json").is_file() or (session / "frames.ncsi").is_file():
            sessions.append(inspect_session(session, calibration, annotations_dir))
    return {"schema_version": 1,
            "generated_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "root": str(root), "sessions": sessions}


def human_catalog(document: dict[str, Any]) -> str:
    lines = ["SESSION | DURATION | ROOM | PLACEMENT | SCENARIO | PATHS | GAPS | CALIBRATION | RAW"]
    for row in document["sessions"]:
        placement = row["placement_label"] or "MISSING"
        lines.append(f"{row['session_id']} | {row['duration_seconds']:.1f}s | {row['room_id']} | "
                     f"{placement} | {row['original_scenario']} | {len(row['paths_present'])}/3 | "
                     f"{row['receiver_sequence_gaps']} | {row['calibration']['status']} | "
                     f"{row['raw_archive']['status']}")
    return "\n".join(lines)
