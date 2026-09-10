"""Stable, small derived evidence contract for VPS/Telegram reporting jobs."""

from __future__ import annotations

import csv
import json
from pathlib import Path
from typing import Any

REPORT_CONTRACT = "newo_csi_derived_report_v1"
ANALYSIS_VERSION = "phase6-v1"
VARIABLE_STATES = {"RF_CHANGE", "MOTION_CANDIDATE"}


def _ranked_events(inference: dict[str, Any]) -> list[dict[str, Any]]:
    events = []
    current = None
    for window in inference.get("windows", []):
        active_paths = sorted(name for name, value in window.get("paths", {}).items()
                              if value.get("state") in VARIABLE_STATES)
        if not active_paths or not window.get("included_in_aggregate"):
            if current:
                events.append(current)
                current = None
            continue
        start = max(0.0, window["elapsed_seconds"] - window["window_duration_seconds"])
        if current is None:
            current = {"start_elapsed_seconds": start, "end_elapsed_seconds": window["elapsed_seconds"],
                       "paths": set(active_paths), "peak_path_count": len(active_paths),
                       "peak_fusion_confidence": window.get("fusion_confidence", 0.0),
                       "cross_node_sync_trustworthy": window.get("synchronization", {}).get(
                           "cross_node_alignment_available", False)}
        else:
            current["end_elapsed_seconds"] = window["elapsed_seconds"]
            current["paths"].update(active_paths)
            current["peak_path_count"] = max(current["peak_path_count"], len(active_paths))
            current["peak_fusion_confidence"] = max(current["peak_fusion_confidence"],
                                                     window.get("fusion_confidence", 0.0))
            current["cross_node_sync_trustworthy"] = (
                current["cross_node_sync_trustworthy"] and
                window.get("synchronization", {}).get("cross_node_alignment_available", False))
    if current:
        events.append(current)
    for event in events:
        event["paths"] = sorted(event["paths"])
        event["duration_seconds"] = round(event["end_elapsed_seconds"] - event["start_elapsed_seconds"], 6)
        event["peak_fusion_confidence"] = round(event["peak_fusion_confidence"], 6)
    return sorted(events, key=lambda row: (-row["peak_path_count"], -row["duration_seconds"]))[:20]


def build_derived_report(evaluation: dict[str, Any]) -> dict[str, Any]:
    metadata = evaluation.get("original_capture_metadata", {})
    inference = evaluation["dsp_inference"]
    paths = {}
    geometry_stable = True
    for name, value in inference.get("paths", {}).items():
        switches = value.get("csi_geometry_switch_count", 0)
        geometry_stable = geometry_stable and switches == 0
        paths[name] = {
            "available": value.get("sample_count", 0) > 0,
            "sample_count": value.get("sample_count", 0),
            "effective_sample_rate_hz": value.get("effective_sample_rate_hz", 0.0),
            "quality_distribution": value.get("signal_quality_distribution", {}),
            "geometry_switch_count": switches,
            "dominant_geometry": value.get("dominant_csi_geometry"),
            "state_fraction": value.get("state_fraction", {}),
        }
    return {
        "schema_version": 1, "contract": REPORT_CONTRACT,
        "session_id": metadata.get("session_id"),
        "capture": {"started_at": metadata.get("capture_started_at"),
                    "ended_at": metadata.get("capture_ended_at"),
                    "duration_seconds": inference.get("duration_seconds", 0.0)},
        "versions": {"firmware": metadata.get("firmware_version"),
                     "host_analysis": ANALYSIS_VERSION},
        "nodes": metadata.get("node_identities", metadata.get("path_mapping")),
        "profile": {"room_id": metadata.get("room_id"),
                    "placement": metadata.get("placement_label"),
                    "geometry_stable": geometry_stable},
        "paths": paths,
        "transport": inference.get("transport", {}),
        "calibration": inference.get("calibration", {}),
        "sync": inference.get("sync", {
            "state": "UNAVAILABLE", "sample_count": 0,
            "interpretation": "no SYNC records; cross-node temporal fusion unavailable",
        }),
        "interval_summary": inference.get("nuisance_patterns", {}),
        "ranked_rf_events": _ranked_events(inference),
        "operator_annotations": evaluation.get("operator_post_hoc_annotations", []),
        "interpretation_boundary": inference.get("interpretation_boundary"),
    }


def write_report_artifacts(report: dict[str, Any], output_dir: Path) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    session_id = report.get("session_id") or "unknown-session"
    json_path = output_dir / f"{session_id}.derived.json"
    csv_path = output_dir / f"{session_id}.paths.csv"
    md_path = output_dir / f"{session_id}.report.md"
    temporary = json_path.with_suffix(json_path.suffix + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(json_path)
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("session_id", "path", "available", "sample_count", "rate_hz", "geometry_switches"))
        for name, path in report["paths"].items():
            writer.writerow((session_id, name, path["available"], path["sample_count"],
                             path["effective_sample_rate_hz"], path["geometry_switch_count"]))
    lines = [f"# Newo CSI report: {session_id}", "",
             f"Duration: {report['capture']['duration_seconds']} s",
             f"Calibration: {report['calibration'].get('status', 'UNAVAILABLE')}",
             f"Synchronization: {report['sync'].get('state', 'UNAVAILABLE')}",
             f"Geometry stable: {report['profile']['geometry_stable']}", "", "## RF paths", ""]
    for name, path in report["paths"].items():
        lines.append(f"- {name}: {path['effective_sample_rate_hz']} Hz; samples={path['sample_count']}; geometry switches={path['geometry_switch_count']}")
    lines += ["", "## Interpretation boundary", "", report.get("interpretation_boundary") or "No inference claim."]
    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return [json_path, csv_path, md_path]
