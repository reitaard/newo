from __future__ import annotations

from dataclasses import asdict
import hashlib
import json
import os
from pathlib import Path
import re
import time

from newo_csi.dsp import CsiPipeline
from newo_csi.protocol import CsiRecord, DiagnosticRecord, PATH_NAMES, ProtocolError, StatusRecord, SyncRecord, decode, mac_text
from newo_csi.sync import SyncAnalyzer

from retrack.nodes import NodeRegistry
from retrack.sessions import GeometryProfile, GeometryState
from retrack.storage import ChunkedSessionWriter
from retrack.topology import Link, Topology


class ReTrackCore:
    """Headless state machine. Raw persistence happens before derived processing."""

    def __init__(self, *, data_dir: Path, registry: NodeRegistry, room: str,
                 placement: str = "UNSPECIFIED", top_k: int = 24,
                 window_seconds: float = 2.0, settle_seconds: float = 10.0,
                 calibration_seconds: float = 30.0,
                 rotate_bytes: int = 64 * 1024 * 1024,
                 calibration_file: Path | None = None, publisher=None):
        self.data_dir = data_dir
        self.registry = registry
        self.room = room
        self.pipeline = CsiPipeline(top_k, window_seconds)
        self.sync = SyncAnalyzer()
        self.topology = Topology()
        self.geometry = GeometryProfile(room, placement)
        self.settle_seconds = settle_seconds
        self.calibration_seconds = max(5.0, float(calibration_seconds))
        self.rotate_bytes = rotate_bytes
        self.publisher = publisher
        self.recorder: ChunkedSessionWriter | None = None
        self.records = 0
        self.rejected = 0
        self.last_record_ns: int | None = None
        self.node_seen_ns: dict[int, int] = {}
        self.track_actual = "OFF"
        self.track_owner = "NONE"
        self.replay_session_id: str | None = None
        self.replay_source: dict[str, object] | None = None
        self.calibration_file = calibration_file
        self.calibration_id: str | None = None
        self.calibration_load_status = "MISSING"
        self.calibration_load_reason = "calibration not configured"
        self.calibration_started_ns: int | None = None
        self.calibration_selection_until_ns: int | None = None
        self.calibration_complete_ns: int | None = None
        self.calibration_failure: str | None = None
        self.teacher_last_observation: dict[str, object] | None = None
        self.load_calibration(calibration_file)

    @property
    def recording(self) -> bool:
        return self.recorder is not None

    def load_calibration(self, path: Path | None) -> dict[str, object]:
        self.calibration_file = path
        self.calibration_id = None
        if path is None:
            self.pipeline.calibration = {}
            self.pipeline.calibration_rejection = "calibration not configured"
            self.calibration_load_status = "MISSING"
            self.calibration_load_reason = self.pipeline.calibration_rejection
            return self.calibration_snapshot()
        try:
            payload = path.read_bytes()
        except OSError:
            self.pipeline.calibration = {}
            self.pipeline.calibration_rejection = f"calibration file missing: {path}"
            self.calibration_load_status = "MISSING"
            self.calibration_load_reason = self.pipeline.calibration_rejection
            return self.calibration_snapshot()
        try:
            document = json.loads(payload.decode("utf-8"))
            if not isinstance(document, dict):
                raise ValueError("top-level JSON must be an object")
        except (UnicodeError, json.JSONDecodeError, ValueError) as error:
            self.pipeline.calibration = {}
            self.pipeline.calibration_rejection = f"calibration file malformed: {error}"
            self.calibration_load_status = "REJECTED"
            self.calibration_load_reason = self.pipeline.calibration_rejection
            return self.calibration_snapshot()
        self.pipeline.load_calibration(document, self.geometry.placement, self.room)
        if self.pipeline.calibration_rejection:
            self.calibration_load_status = "REJECTED"
            self.calibration_load_reason = self.pipeline.calibration_rejection
        else:
            declared_id = document.get("calibration_id")
            self.calibration_id = (declared_id if isinstance(declared_id, str)
                                   and re.fullmatch(r"[0-9a-f]{16}", declared_id)
                                   else hashlib.sha256(payload).hexdigest()[:16])
            self.calibration_load_status = "PENDING"
            self.calibration_load_reason = "awaiting CSI geometry"
        return self.calibration_snapshot()

    def calibration_snapshot(self) -> dict[str, object]:
        report = self.pipeline.calibration_report()
        status = report.get("status", self.calibration_load_status)
        reason = report.get("reason") or self.calibration_load_reason
        if self.pipeline.calibration_stage in ("selection", "baseline"):
            status = ("BUILDING_SELECTION" if self.pipeline.calibration_stage == "selection"
                      else "BUILDING_BASELINE")
            reason = self.calibration_load_reason
        elif self.calibration_load_status == "REQUIRED":
            status = "REQUIRED"
            reason = self.calibration_load_reason
        elif self.pipeline.calibration_rejection:
            rejection = self.pipeline.calibration_rejection
            if "not configured" in rejection or "file missing" in rejection:
                status = "MISSING"
            elif "placement changed" in rejection:
                status = "REQUIRED"
            else:
                status = "REJECTED"
            reason = rejection
        elif status in ("MATCH", "PARTIAL"):
            status = "VALID" if status == "MATCH" else "PARTIAL"
            if report.get("status") == "PARTIAL":
                status = "PARTIAL"
                reason = report.get("reason")
            else:
                reason = None
            if (report.get("matched_paths")
                    and self.geometry.state not in (GeometryState.REPOSITIONING, GeometryState.SETTLING,
                                                    GeometryState.BUILDING_SELECTION,
                                                    GeometryState.BUILDING_BASELINE)):
                self.geometry.ready(self.calibration_id or "loaded-calibration")
        elif status == "REJECTED" and self.geometry.state == GeometryState.READY:
            self.geometry.state = GeometryState.CALIBRATION_REQUIRED
            self.geometry.calibration_id = None
        return {
            "status": status,
            "reason": reason,
            "calibration_id": self.calibration_id,
            "file": str(self.calibration_file) if self.calibration_file else None,
            "room": report.get("room_id"),
            "placement": report.get("placement"),
            "matched_paths": report.get("matched_paths", []),
            "mismatched_geometries": report.get("mismatched_geometries", []),
            "unmatched_dominant_paths": report.get("unmatched_dominant_paths", []),
            "stage": self.pipeline.calibration_stage,
            "failure": self.calibration_failure,
        }

    def _default_calibration_path(self) -> Path:
        safe = lambda value: re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip()).strip("._") or "UNSPECIFIED"
        return self.data_dir / "calibrations" / f"{safe(self.room)}--{safe(self.geometry.placement)}.json"

    def start_calibration(self, *, duration_seconds: float | None = None,
                          now_ns: int | None = None) -> None:
        value = time.monotonic_ns() if now_ns is None else now_ns
        if self.replay_source is None and self.track_actual != "ON":
            raise RuntimeError("Track must be ON before calibration")
        self.geometry.update(value)
        self.geometry.begin_calibration()
        duration = max(5.0, self.calibration_seconds if duration_seconds is None else duration_seconds)
        self.pipeline.calibration = {}
        self.pipeline.calibration_rejection = None
        self.pipeline.begin_calibration()
        self.calibration_started_ns = value
        self.calibration_selection_until_ns = value + int(duration * 0.4 * 1e9)
        self.calibration_complete_ns = value + int(duration * 1e9)
        self.calibration_load_status = "BUILDING_SELECTION"
        self.calibration_load_reason = "quiet baseline requested by operator"
        self.calibration_failure = None
        if self.recorder is not None:
            self.recorder.event("calibration_started", monotonic_ns=value,
                                room=self.room, placement=self.geometry.placement,
                                duration_seconds=duration)

    def cancel_calibration(self, *, reason: str = "cancelled by operator",
                           now_ns: int | None = None) -> None:
        if self.pipeline.calibration_stage is None:
            return
        self._fail_calibration(reason, now_ns=now_ns, event="calibration_cancelled")

    def _fail_calibration(self, reason: str, *, now_ns: int | None = None,
                          event: str = "calibration_failed") -> None:
        value = time.monotonic_ns() if now_ns is None else now_ns
        self.pipeline.calibration_stage = None
        self.pipeline.calibration_stats = None
        self.pipeline.calibration_frozen_geometries = set()
        self.calibration_started_ns = None
        self.calibration_selection_until_ns = None
        self.calibration_complete_ns = None
        self.calibration_load_status = "REQUIRED"
        self.calibration_load_reason = reason
        self.calibration_failure = reason
        self.geometry.require_calibration()
        if self.recorder is not None:
            self.recorder.event(event, monotonic_ns=value, reason=reason)

    def _write_calibration(self, document: dict[str, object]) -> Path:
        target = self.calibration_file or self._default_calibration_path()
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(target.suffix + ".tmp")
        payload = json.dumps(document, indent=2, sort_keys=True) + "\n"
        temporary.write_text(payload, encoding="utf-8")
        validator = CsiPipeline(self.pipeline.top_k, self.pipeline.window_seconds)
        validator.load_calibration(document, self.geometry.placement, self.room)
        if validator.calibration_rejection:
            temporary.unlink(missing_ok=True)
            raise ValueError(validator.calibration_rejection)
        os.replace(temporary, target)
        return target

    def _advance_calibration(self, now_ns: int) -> None:
        stage = self.pipeline.calibration_stage
        if stage == "selection" and self.calibration_selection_until_ns is not None \
                and now_ns >= self.calibration_selection_until_ns:
            try:
                self.pipeline.freeze_calibration_selection()
                if not self.pipeline.calibration_frozen_geometries:
                    raise ValueError("no usable CSI geometry during selection stage")
                self.geometry.baseline_calibration()
                self.calibration_load_status = "BUILDING_BASELINE"
                self.calibration_load_reason = "frozen selection; collecting quiet baseline"
                if self.recorder is not None:
                    self.recorder.event("calibration_selection_frozen", monotonic_ns=now_ns,
                                        geometries=sorted(self.pipeline.calibration_frozen_geometries))
            except ValueError as error:
                self.cancel_calibration(reason=str(error), now_ns=now_ns)
                return
        if self.pipeline.calibration_stage == "baseline" and self.calibration_complete_ns is not None \
                and now_ns >= self.calibration_complete_ns:
            try:
                document = self.pipeline.calibration_document(self.geometry.placement, self.room)
                if not document.get("paths"):
                    raise ValueError("insufficient baseline samples for calibration")
                target = self._write_calibration(document)
                self.pipeline.calibration_stage = None
                self.pipeline.calibration_stats = None
                self.pipeline.calibration_frozen_geometries = set()
                self.calibration_started_ns = None
                self.calibration_selection_until_ns = None
                self.calibration_complete_ns = None
                self.geometry.require_calibration()
                # The generated document is accepted only through the ordinary loader.
                result = self.load_calibration(target)
                if result["status"] not in ("PENDING", "VALID", "PARTIAL"):
                    raise ValueError(str(result.get("reason") or "calibration reload failed"))
                if self.recorder is not None:
                    self.recorder.set_calibration(result)
                    self.recorder.event("calibration_completed", monotonic_ns=now_ns,
                                        calibration_id=self.calibration_id, file=str(target))
            except (OSError, ValueError) as error:
                self._fail_calibration(str(error), now_ns=now_ns)

    def calibration_progress(self, now_ns: int) -> dict[str, object]:
        stage = self.pipeline.calibration_stage
        remaining = None if self.calibration_complete_ns is None else max(
            0.0, (self.calibration_complete_ns - now_ns) / 1e9)
        return {"active": stage is not None,
                "stage": ({"selection": "BUILDING_SELECTION", "baseline": "BUILDING_BASELINE"}.get(stage)),
                "remaining_seconds": remaining,
                "frozen_geometries": sorted(self.pipeline.calibration_frozen_geometries)}

    def _register_record_node(self, record, source_ip: str, host_ns: int) -> None:
        node_number = getattr(record, "node_id", None)
        receiver = getattr(record, "receiver_mac", None)
        if node_number is None or receiver is None:
            return
        name = "Newo" if node_number == 1 else ("Newo2" if node_number == 2 else f"Newo{node_number}")
        role = "LEADER" if node_number == 1 else "FOLLOWER"
        mac = mac_text(receiver)
        node = self.registry.get_by_mac(mac)
        if node is None or node.last_ip != source_ip:
            node = self.registry.register(mac, friendly_name=name, role=role,
                                          capabilities=("CSI", "ESP-NOW"), last_ip=source_ip,
                                          last_seen_ns=host_ns, room=self.room,
                                          placement=self.geometry.placement,
                                          sync_role=role)
        else:
            # Last-seen is hot runtime state. Persisting it for every CSI frame
            # would put registry I/O in the raw collection path.
            node.last_seen_ns = host_ns
        self.node_seen_ns[node_number] = host_ns

    def ingest(self, raw: bytes, host_monotonic_ns: int, host_wall_ns: int,
               source: tuple[str, int]):
        try:
            record = decode(raw)
        except ProtocolError:
            self.rejected += 1
            return None
        if self.recorder is not None:
            self.recorder.append(raw, host_monotonic_ns, host_wall_ns, source)
        self.records += 1
        self.last_record_ns = host_monotonic_ns
        self._register_record_node(record, source[0], host_monotonic_ns)
        if isinstance(record, CsiRecord):
            self.pipeline.add(record, host_monotonic_ns)
            self.topology.upsert(Link(
                link_id=f"csi:{mac_text(record.source_mac)}>{mac_text(record.receiver_mac)}",
                source=mac_text(record.source_mac), receiver=mac_text(record.receiver_mac),
                directed=True, health="ONLINE", last_seen_ns=host_monotonic_ns))
        elif isinstance(record, SyncRecord):
            self.sync.add(record, host_monotonic_ns)
        elif isinstance(record, (StatusRecord, DiagnosticRecord)):
            boot = getattr(record, "boot_id", None)
            node = self.registry.get_by_mac(mac_text(record.receiver_mac))
            if node is not None and boot is not None:
                node.boot_session_id = boot
                self.registry.save()
        if self.publisher is not None:
            self.publisher(self.snapshot(host_monotonic_ns))
        return record

    def start_recording(self, label: str | None = None) -> ChunkedSessionWriter:
        if self.recorder is not None:
            return self.recorder
        self.recorder = ChunkedSessionWriter(
            self.data_dir / "sessions", room=self.room,
            nodes=[asdict(node) for node in self.registry.nodes], topology=self.topology.as_list(),
            placement=self.geometry.placement, rotate_bytes=self.rotate_bytes,
            calibration=self.calibration_snapshot())
        if label:
            self.recorder.event("session_label", value=label)
        return self.recorder

    def stop_recording(self, state: str = "COMPLETE") -> Path | None:
        if self.recorder is None:
            return None
        directory = self.recorder.directory
        self.recorder.close(state)
        self.recorder = None
        return directory

    def add_event(self, label: str, note: str | None = None, *, monotonic_ns: int | None = None) -> None:
        if self.recorder is None:
            raise RuntimeError("recording is not active")
        self.recorder.event(label, monotonic_ns=monotonic_ns, note=note,
                            placement=self.geometry.placement)

    def add_teacher_observation(self, observation: str, note: str | None = None,
                                *, monotonic_ns: int | None = None,
                                source: str = "CAMERA_TEACHER") -> None:
        allowed = {"NO_PERSON_VISIBLE", "PERSON_VISIBLE", "PERSON_STILL", "PERSON_MOVING",
                   "ENTER_FRAME", "EXIT_FRAME"}
        if observation not in allowed:
            raise ValueError("unsupported teacher observation")
        if self.recorder is None:
            raise RuntimeError("recording is not active")
        value = time.monotonic_ns() if monotonic_ns is None else monotonic_ns
        self.teacher_last_observation = {"observation": observation, "source": source,
                                         "host_monotonic_ns": value}
        self.recorder.event("teacher_observation", monotonic_ns=value,
                            evidence_type="GROUND_TRUTH", source=source,
                            observation=observation, note=note,
                            placement=self.geometry.placement,
                            rf_inference_overwritten=False)

    def change_placement(self, placement: str, *, now_ns: int | None = None) -> None:
        value = time.monotonic_ns() if now_ns is None else now_ns
        if self.recorder is not None:
            self.recorder.event("node_moved", monotonic_ns=value,
                                old_placement=self.geometry.placement, new_placement=placement)
        if self.pipeline.calibration_stage is not None:
            self.cancel_calibration(reason="placement changed during calibration", now_ns=value)
        self.geometry.move(placement, value, self.settle_seconds)
        self.pipeline.calibration = {}
        self.pipeline.calibration_rejection = "placement changed; recalibration required"
        self.calibration_load_status = "REQUIRED"
        self.calibration_load_reason = self.pipeline.calibration_rejection
        self.calibration_id = None
        for processor in self.pipeline.processors.values():
            processor.baseline = None

    def snapshot(self, now_ns: int | None = None) -> dict[str, object]:
        now_ns = time.monotonic_ns() if now_ns is None else now_ns
        self.geometry.update(now_ns)
        self._advance_calibration(now_ns)
        calibration = self.calibration_snapshot()
        interpretation_valid = (calibration["status"] in ("VALID", "PARTIAL")
                                and self.geometry.state == GeometryState.READY
                                and (self.track_actual == "ON" or self.replay_source is not None))
        paths = {}
        for path_id, item in self.pipeline.snapshots().items():
            paths[PATH_NAMES.get(path_id, str(path_id))] = {
                "hz": item.sample_rate_hz, "rssi_dbm": item.rssi_dbm,
                "quality": item.signal_quality,
                "score": item.motion_score if interpretation_valid and item.calibrated else None,
                "state": item.motion_state if interpretation_valid and item.calibrated else "LOW_CONFIDENCE",
                "calibration": "EXACT" if item.calibrated else "UNMATCHED",
                "geometry": item.geometry,
                "selected_subcarriers": item.selected_subcarriers,
                "sequence": item.last_sequence,
            }
        geometry_partitions: dict[str, list[dict[str, object]]] = {}
        for geometry, count in self.pipeline.geometry_counts.items():
            dominant = self.pipeline.dominant(geometry.path_id)
            geometry_partitions.setdefault(PATH_NAMES.get(geometry.path_id, str(geometry.path_id)), []).append({
                "identity": geometry.identity, "records": count,
                "calibration": "EXACT" if geometry.identity in self.pipeline.calibration else "UNMATCHED",
                "dominant": dominant is not None and dominant.geometry == geometry,
            })
        for values in geometry_partitions.values():
            values.sort(key=lambda item: (-int(item["records"]), str(item["identity"])))
        fusion_state, fusion_confidence = self.pipeline.fused(not interpretation_valid)
        return {
            "schema": "retrack_runtime_snapshot_v1", "room": self.room,
            "track": self.track_actual, "owner": self.track_owner,
            "recording": self.recording, "records": self.records, "rejected": self.rejected,
            "recording_elapsed_seconds": (
                max(0.0, (now_ns - self.recorder.origin_ns) / 1e9) if self.recorder else 0.0),
            "session_id": self.recorder.session_id if self.recorder else self.replay_session_id,
            "replay_source": self.replay_source,
            "geometry": {"placement": self.geometry.placement, "state": self.geometry.state.value,
                         "calibration_id": self.geometry.calibration_id},
            "nodes": [asdict(node) for node in self.registry.nodes],
            "paths": paths, "geometry_partitions": geometry_partitions,
            "topology": self.topology.as_list(), "sync": self.sync.summary(),
            "calibration": calibration,
            "calibration_progress": self.calibration_progress(now_ns),
            "fusion": {"state": fusion_state, "confidence": fusion_confidence},
            "teacher": {"status": "AVAILABLE" if self.teacher_last_observation else "ABSENT",
                        "last_observation": self.teacher_last_observation,
                        "runtime_dependency": False},
            "publisher": "OPTIONAL" if self.publisher is not None else "DISABLED",
        }
