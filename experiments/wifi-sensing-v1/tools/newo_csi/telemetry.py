"""Bounded, best-effort collector telemetry for the VPS Track panel.

Raw NCSI records never cross this interface.  The same LiveState is used by
live UDP capture and archive replay, so this is derived presentation evidence.
"""

from __future__ import annotations

import json
import queue
import threading
import time
from typing import Any
from urllib.request import Request, urlopen

from .protocol import PATH_NAMES

SCHEMA = "newo_track_telemetry_v1"


def build_track_snapshot(state: Any, recorder: Any | None, *, room_id: str,
                         collector: str, now_monotonic: float | None = None,
                         now_wall_ns: int | None = None) -> dict[str, Any]:
    now = time.monotonic() if now_monotonic is None else now_monotonic
    wall_ns = time.time_ns() if now_wall_ns is None else now_wall_ns
    calibration = state.pipeline.calibration_report()
    fusion, confidence = state.pipeline.fused(state.repositioning)
    sync = state.sync.summary()
    snapshots = state.pipeline.snapshots()
    paths = []
    total_samples = total_gaps = 0
    for path_id in (1, 2, 3):
        item = snapshots.get(path_id)
        if item is None:
            paths.append({"path_id": path_id, "name": PATH_NAMES[path_id],
                          "available": False})
            continue
        total_samples += item.samples
        total_gaps += item.sequence_gaps
        paths.append({
            "path_id": path_id, "name": item.path_name, "available": True,
            "hz": round(item.sample_rate_hz, 3), "rssi_dbm": item.rssi_dbm,
            "quality": item.signal_quality, "score": (None if item.motion_score is None
                                                          else round(item.motion_score, 6)),
            "state": item.motion_state, "selected_subcarriers": item.selected_subcarriers,
            "samples": item.samples, "sequence": item.last_sequence,
            "sequence_gaps": item.sequence_gaps, "duplicates": item.duplicates,
            "geometry": item.geometry,
        })
    receiver_loss = [{"node_id": node, "receiver_mac": receiver, **values}
                     for (node, receiver), values in state.pipeline.receiver_losses().items()]
    geometry_state = getattr(state, "geometry_state", None) or (
        "REPOSITIONING" if state.repositioning else ("READY" if snapshots else "UNKNOWN"))
    calibration_status = ("BUILDING" if getattr(state.pipeline, "calibration_stage", None)
                          in ("selection", "baseline") else calibration.get("status", "UNKNOWN"))
    record_elapsed = (None if recorder is None else
                      round(max(0.0, now - recorder.started_monotonic), 3))
    node_age = {str(node): round(max(0.0, now - seen), 3)
                for node, seen in state.node_seen.items()}
    return {
        "schema": SCHEMA, "generated_at_unix_ns": wall_ns,
        "room": room_id, "placement": state.placement,
        "geometry_state": geometry_state,
        "collector": {"kind": collector, "state": state.collector_state,
                      "recording": recorder is not None,
                      "session_id": getattr(recorder, "session_id", None),
                      "recording_elapsed_s": record_elapsed,
                      "records": state.records, "rejected": state.rejected},
        "nodes": {
            "newo": {"online": now - state.node_seen.get(1, -1e9) < 5,
                     "age_s": node_age.get("1")},
            "newo2": {"online": now - state.node_seen.get(2, -1e9) < 5,
                      "age_s": node_age.get("2")},
        },
        "paths": paths,
        "path_summary": {"available": sum(bool(p["available"]) for p in paths),
                         "expected": 3,
                         "loss_percent": (None if total_samples + total_gaps == 0 else
                                          round(100.0 * total_gaps /
                                                (total_samples + total_gaps), 4))},
        "receiver_loss": receiver_loss,
        "fusion": {"state": fusion, "confidence": round(confidence, 6)},
        "calibration": {"status": calibration_status,
                        "reason": calibration.get("reason"),
                        "room": calibration.get("room_id"),
                        "placement": calibration.get("placement"),
                        "schema_version": calibration.get("schema_version")},
        "sync": {key: sync.get(key) for key in (
            "state", "sample_count", "accepted_samples", "rejected_samples",
            "final_offset_us", "drift_ppm", "last_sync_age_us", "jitter_us",
            "leader_session_id", "follower_session_id", "session_epoch_count",
            "residual_us")},
        "device_diagnostics": {
            str(node): {
                "boot_id": status.boot_id, "callbacks_total": status.callbacks_total,
                "ring_drops": status.ring_full_drops, "transport_ok": status.transport_ok,
                "transport_drops": status.transport_drops,
                **({} if node not in getattr(state, "latest_diagnostic", {}) else {
                    "probe_tx_attempted": state.latest_diagnostic[node].probe_tx_attempted,
                    "probe_tx_success": state.latest_diagnostic[node].probe_tx_success,
                    "probe_tx_link_failure": state.latest_diagnostic[node].probe_tx_link_failure,
                    "probe_rx_valid": state.latest_diagnostic[node].probe_rx_valid,
                    "probe_rx_invalid": state.latest_diagnostic[node].probe_rx_invalid,
                }),
            } for node, status in getattr(state, "latest_status", {}).items()
        },
    }


class TelemetryPublisher:
    """A single-latest-slot HTTP publisher; collector processing never waits."""

    def __init__(self, url: str | None, token: str | None, timeout: float = 1.5):
        self.url, self.token, self.timeout = url, token, timeout
        self._queue: queue.Queue[dict[str, Any] | None] = queue.Queue(maxsize=1)
        self._thread: threading.Thread | None = None
        if url:
            self._thread = threading.Thread(target=self._run, name="track-telemetry",
                                            daemon=True)
            self._thread.start()

    def submit(self, snapshot: dict[str, Any]) -> None:
        if self._thread is None:
            return
        try:
            self._queue.put_nowait(snapshot)
        except queue.Full:
            try:
                self._queue.get_nowait()
            except queue.Empty:
                pass
            try:
                self._queue.put_nowait(snapshot)
            except queue.Full:
                pass

    def _run(self) -> None:
        while True:
            value = self._queue.get()
            if value is None:
                return
            body = json.dumps(value, separators=(",", ":")).encode("utf-8")
            # A named client also avoids generic urllib bot filtering at the
            # public reverse-proxy edge while remaining ordinary HTTP.
            headers = {"Content-Type": "application/json",
                       "User-Agent": "NewoCSI-Collector/1.0"}
            if self.token:
                headers["Authorization"] = f"Bearer {self.token}"
            try:
                with urlopen(Request(self.url, data=body, headers=headers,
                                     method="POST"), timeout=self.timeout) as response:
                    response.read(1)
            except OSError:
                pass

    def close(self) -> None:
        if self._thread is None:
            return
        try:
            self._queue.put_nowait(None)
        except queue.Full:
            try:
                self._queue.get_nowait()
                self._queue.put_nowait(None)
            except queue.Empty:
                pass
        self._thread.join(timeout=self.timeout + .5)
        self._thread = None
