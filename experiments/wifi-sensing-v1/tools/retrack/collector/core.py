from __future__ import annotations

from dataclasses import asdict
from pathlib import Path
import time

from newo_csi.dsp import CsiPipeline
from newo_csi.protocol import CsiRecord, DiagnosticRecord, PATH_NAMES, ProtocolError, StatusRecord, SyncRecord, decode, mac_text
from newo_csi.sync import SyncAnalyzer

from retrack.nodes import NodeRegistry
from retrack.sessions import GeometryProfile
from retrack.storage import ChunkedSessionWriter
from retrack.topology import Link, Topology


class ReTrackCore:
    """Headless state machine. Raw persistence happens before derived processing."""

    def __init__(self, *, data_dir: Path, registry: NodeRegistry, room: str,
                 placement: str = "UNSPECIFIED", top_k: int = 24,
                 window_seconds: float = 2.0, settle_seconds: float = 10.0,
                 rotate_bytes: int = 64 * 1024 * 1024, publisher=None):
        self.data_dir = data_dir
        self.registry = registry
        self.room = room
        self.pipeline = CsiPipeline(top_k, window_seconds)
        self.sync = SyncAnalyzer()
        self.topology = Topology()
        self.geometry = GeometryProfile(room, placement)
        self.settle_seconds = settle_seconds
        self.rotate_bytes = rotate_bytes
        self.publisher = publisher
        self.recorder: ChunkedSessionWriter | None = None
        self.records = 0
        self.rejected = 0
        self.last_record_ns: int | None = None
        self.node_seen_ns: dict[int, int] = {}
        self.track_actual = "OFF"
        self.track_owner = "NONE"

    @property
    def recording(self) -> bool:
        return self.recorder is not None

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
            placement=self.geometry.placement, rotate_bytes=self.rotate_bytes)
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

    def change_placement(self, placement: str, *, now_ns: int | None = None) -> None:
        value = time.monotonic_ns() if now_ns is None else now_ns
        if self.recorder is not None:
            self.recorder.event("node_moved", monotonic_ns=value,
                                old_placement=self.geometry.placement, new_placement=placement)
        self.geometry.move(placement, value, self.settle_seconds)
        self.pipeline.calibration = {}
        self.pipeline.calibration_rejection = "placement changed; recalibration required"
        for processor in self.pipeline.processors.values():
            processor.baseline = None

    def snapshot(self, now_ns: int | None = None) -> dict[str, object]:
        now_ns = time.monotonic_ns() if now_ns is None else now_ns
        self.geometry.update(now_ns)
        paths = {}
        for path_id, item in self.pipeline.snapshots().items():
            paths[PATH_NAMES.get(path_id, str(path_id))] = {
                "hz": item.sample_rate_hz, "rssi_dbm": item.rssi_dbm,
                "quality": item.signal_quality, "score": item.motion_score,
                "state": item.motion_state, "selected_subcarriers": item.selected_subcarriers,
                "sequence": item.last_sequence,
            }
        return {
            "schema": "retrack_runtime_snapshot_v1", "room": self.room,
            "track": self.track_actual, "owner": self.track_owner,
            "recording": self.recording, "records": self.records, "rejected": self.rejected,
            "session_id": self.recorder.session_id if self.recorder else None,
            "geometry": {"placement": self.geometry.placement, "state": self.geometry.state.value,
                         "calibration_id": self.geometry.calibration_id},
            "nodes": [asdict(node) for node in self.registry.nodes],
            "paths": paths, "topology": self.topology.as_list(), "sync": self.sync.summary(),
            "publisher": "OPTIONAL" if self.publisher is not None else "DISABLED",
        }
