from __future__ import annotations

from dataclasses import asdict
import json
from pathlib import Path
import socket
import struct
import tempfile
import threading
import time
import unittest

from newo_csi.archive import ArchiveWriter
from newo_csi.dsp import CsiPipeline
from newo_csi.protocol import crc32c, decode
from retrack.api import ClientBroker, ReTrackClient
from retrack.collector import NetworkRuntime, ReTrackCore
from retrack.control import ControlAck, LocalTrackClient
from retrack.daemon import ReTrackDaemon
from retrack.nodes import NodeRegistry
from retrack.replay import replay_session
from retrack.sessions import GeometryState


def csi_frame(sequence: int, *, node: int = 1, path: int = 1,
              receiver: bytes = b"\x10\x11\x12\x13\x14\x15",
              source: bytes = b"\x20\x21\x22\x23\x24\x25",
              iq_length: int = 64) -> bytes:
    iq = bytes((index + sequence) % 127 for index in range(iq_length))
    length = 88 + len(iq)
    output = bytearray(length)
    struct.pack_into("<4sBBHII", output, 0, b"NCSI", 1, 1, 88, length, 0)
    struct.pack_into("<I6s6sIQBBBBbbBBHHHHHBB", output, 16, node, receiver, source,
                     sequence, sequence * 20_000, 6, 0, 0, 1, -55, -91, 0, 3,
                     len(iq), len(iq), len(iq) // 2, 0x04, path, 0, 1)
    struct.pack_into("<IBBHBBHHH6sH", output, 64, sequence * 20_000, 0, 4, 0x8e,
                     2, 0, 120, sequence & 0xFFF, 0, receiver, 0)
    output[88:] = iq
    struct.pack_into("<I", output, 12, crc32c(output))
    return bytes(output)


def build_calibration(path: Path, placement: str = "DESK", room: str = "bedroom") -> None:
    pipeline = CsiPipeline(top_k=4, window_seconds=2)
    pipeline.begin_calibration()
    for sequence in range(1, 31):
        pipeline.add(decode(csi_frame(sequence)), sequence * 20_000_000)
    pipeline.freeze_calibration_selection()
    for sequence in range(31, 101):
        pipeline.add(decode(csi_frame(sequence)), sequence * 20_000_000)
    path.write_text(json.dumps(pipeline.calibration_document(placement, room)), encoding="utf-8")


class FakeRuntime:
    def __init__(self, core):
        self.core = core
        self.closed = False
        self.sequence = 0
        self.emit = False

    def discover_leader(self):
        return ControlAck("fake", 1, True, self.core.track_actual, self.core.track_owner,
                          node_id="Newo", hardware_mac="7c:4f:ad:2b:3c:68")

    def set_track(self, enabled: bool):
        self.core.track_actual = "ON" if enabled else "OFF"
        self.core.track_owner = "LOCAL" if enabled else "NONE"
        return ControlAck("fake", 1, True, self.core.track_actual, self.core.track_owner)

    def poll(self):
        time.sleep(0.002)
        if self.emit:
            self.sequence += 1
            now = self.sequence * 20_000_000
            return self.core.ingest(csi_frame(self.sequence), now, now, ("192.168.1.58", 5005))
        return None

    def close(self):
        self.closed = True


class DaemonHarness:
    def __init__(self, directory: str):
        core = ReTrackCore(data_dir=Path(directory), registry=NodeRegistry(), room="bedroom",
                           placement="DESK", top_k=4, rotate_bytes=2048)
        self.runtime = FakeRuntime(core)
        self.daemon = ReTrackDaemon(self.runtime, api_port=0, snapshot_interval=0.01,
                                    controller_lease_ms=5_000)
        self.daemon.start(discover=False)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        while not self.daemon.closed:
            self.daemon.step()

    @property
    def address(self):
        return self.daemon.api.address

    def close(self):
        self.daemon.close()
        self.thread.join(timeout=1)


class ClientBoundaryTests(unittest.TestCase):
    @staticmethod
    def wait_until(client, predicate, timeout=2.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            snapshot = client.snapshot()
            if predicate(snapshot):
                return snapshot
            time.sleep(0.01)
        raise AssertionError("snapshot condition was not reached")

    def test_multiple_clients_detach_and_reconnect_preserve_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            harness = DaemonHarness(directory)
            host, port = harness.address
            controller = ReTrackClient(host, port, client_id="laptop")
            viewer = ReTrackClient(host, port, client_id="phone")
            try:
                self.assertTrue(controller.acquire_control()["ok"])
                self.assertTrue(controller.mutate("TRACK_SET", state="ON")["ok"])
                started = controller.mutate("RECORD_SET", state="ON")
                self.assertTrue(started["ok"])
                session_id = started["session_id"]
                harness.runtime.emit = True
                first = self.wait_until(controller, lambda item: item.get("session_id") == session_id)
                phone = self.wait_until(viewer, lambda item: item.get("session_id") == session_id)
                self.assertEqual(first["session_id"], session_id)
                self.assertEqual(phone["session_id"], session_id)
                self.assertFalse(viewer.request("RECORD_SET", state="OFF", lease_id="invalid")["ok"])
                before = phone["records"]
                controller.close()
                self.assertTrue(harness.daemon.core.recording)
                later = self.wait_until(viewer, lambda item: item.get("records", 0) > before)
                self.assertEqual(later["session_id"], session_id)
                self.assertGreater(later["records"], before)

                reconnected = ReTrackClient(host, port, client_id="laptop")
                try:
                    self.assertTrue(reconnected.acquire_control()["ok"])
                    current = self.wait_until(
                        reconnected, lambda item: item.get("session_id") == session_id)
                    self.assertEqual(current["session_id"], session_id)
                    request_id = "one-event"
                    first_event = reconnected.request(
                        "EVENT", request_id=request_id, lease_id=reconnected.lease_id,
                        label="CUSTOM", note="test")
                    duplicate = reconnected.request(
                        "EVENT", request_id=request_id, lease_id=reconnected.lease_id,
                        label="CUSTOM", note="test")
                    self.assertTrue(first_event["ok"])
                    self.assertTrue(duplicate["duplicate"])
                    events = (harness.daemon.core.recorder.directory / "events.jsonl").read_text()
                    self.assertEqual(events.count('"label": "CUSTOM"'), 1)
                finally:
                    reconnected.close()
            finally:
                viewer.close()
                harness.close()

    def test_bounded_controller_lease_and_deliberate_takeover(self):
        clock = [10.0]
        calls = []
        broker = ClientBroker(lambda message: calls.append(message) or {},
                              controller_lease_ms=5_000, clock=lambda: clock[0])
        broker.connect("laptop")
        broker.connect("phone")
        laptop = broker.handle("laptop", {"type": "ACQUIRE_CONTROL", "request_id": "a"})
        self.assertTrue(laptop["ok"])
        busy = broker.handle("phone", {"type": "ACQUIRE_CONTROL", "request_id": "b"})
        self.assertEqual(busy["error"], "controller_busy")
        takeover = broker.handle("phone", {
            "type": "ACQUIRE_CONTROL", "request_id": "c", "takeover": True})
        self.assertTrue(takeover["ok"])
        rejected = broker.handle("laptop", {
            "type": "TRACK_SET", "request_id": "d", "lease_id": laptop["lease_id"], "state": "ON"})
        self.assertEqual(rejected["error"], "not_controller")
        clock[0] += 6
        self.assertIsNone(broker.controller_status()["controller_id"])
        self.assertEqual(calls, [])

    def test_daemon_restart_recovers_active_manifest_without_raw_rewrite(self):
        with tempfile.TemporaryDirectory() as directory:
            core = ReTrackCore(data_dir=Path(directory), registry=NodeRegistry(), room="bedroom")
            writer = core.start_recording()
            core.ingest(csi_frame(1), 10, 20, ("192.168.1.58", 5005))
            writer._archive.flush()
            raw_path = writer.directory / "frames-000001.ncsi"
            before = raw_path.read_bytes()
            writer._archive.close()
            writer._events.close()
            restarted_core = ReTrackCore(data_dir=Path(directory), registry=NodeRegistry(), room="bedroom")
            daemon = ReTrackDaemon(FakeRuntime(restarted_core), api_port=0)
            try:
                daemon.start(discover=False)
                manifest = json.loads((writer.directory / "manifest.json").read_text())
                self.assertEqual(manifest["state"], "INCOMPLETE/RECOVERED")
                self.assertEqual(raw_path.read_bytes(), before)
            finally:
                daemon.close()


class CalibrationLoadingTests(unittest.TestCase):
    def test_valid_missing_malformed_and_placement_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            valid = root / "calibration.json"
            build_calibration(valid)
            core = ReTrackCore(data_dir=root, registry=NodeRegistry(), room="bedroom",
                               placement="DESK", top_k=4, calibration_file=valid)
            core.track_actual = "ON"
            for sequence in range(101, 161):
                core.ingest(csi_frame(sequence), sequence * 20_000_000,
                            sequence * 20_000_000, ("192.168.1.58", 5005))
            snapshot = core.snapshot(3_200_000_000)
            self.assertEqual(snapshot["calibration"]["status"], "VALID")
            self.assertEqual(snapshot["geometry"]["state"], "READY")
            self.assertIsNotNone(snapshot["paths"]["ROUTER_NEWO"]["score"])

            missing = ReTrackCore(data_dir=root, registry=NodeRegistry(), room="bedroom",
                                  calibration_file=root / "missing.json")
            self.assertEqual(missing.snapshot()["calibration"]["status"], "MISSING")
            malformed_path = root / "bad.json"
            malformed_path.write_text("[not-json", encoding="utf-8")
            malformed = ReTrackCore(data_dir=root, registry=NodeRegistry(), room="bedroom",
                                    calibration_file=malformed_path)
            self.assertEqual(malformed.snapshot()["calibration"]["status"], "REJECTED")
            mismatch = ReTrackCore(data_dir=root, registry=NodeRegistry(), room="bedroom",
                                   placement="OTHER", top_k=4, calibration_file=valid)
            self.assertEqual(mismatch.snapshot()["calibration"]["status"], "REJECTED")
            self.assertEqual(mismatch.snapshot()["calibration"]["reason"], "placement mismatch")

    def test_secondary_geometry_is_partial_without_suppressing_calibrated_dominant(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            calibration = root / "calibration.json"
            build_calibration(calibration)
            core = ReTrackCore(data_dir=root, registry=NodeRegistry(), room="bedroom",
                               placement="DESK", top_k=4, calibration_file=calibration)
            core.track_actual = "ON"
            for sequence in range(101, 161):
                core.ingest(csi_frame(sequence), sequence * 20_000_000,
                            sequence * 20_000_000, ("192.168.1.58", 5005))
            self.assertEqual(core.snapshot(3_200_000_000)["calibration"]["status"], "VALID")

            core.ingest(csi_frame(161, iq_length=66), 3_220_000_000,
                        3_220_000_000, ("192.168.1.58", 5005))
            snapshot = core.snapshot(3_220_000_001)
            self.assertEqual(snapshot["calibration"]["status"], "PARTIAL")
            self.assertEqual(snapshot["geometry"]["state"], GeometryState.READY.value)
            self.assertIsNotNone(snapshot["paths"]["ROUTER_NEWO"]["score"])
            self.assertEqual(snapshot["paths"]["ROUTER_NEWO"]["calibration"], "EXACT")
            extras = snapshot["geometry_partitions"]["ROUTER_NEWO"]
            self.assertEqual(len(extras), 2)

    def test_placement_invalidates_loaded_calibration_and_replay_shares_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            calibration = root / "calibration.json"
            build_calibration(calibration)
            live = ReTrackCore(data_dir=root, registry=NodeRegistry(), room="bedroom",
                               placement="DESK", top_k=4, calibration_file=calibration)
            live.track_actual = "ON"
            live.start_recording()
            for sequence in range(101, 161):
                live.ingest(csi_frame(sequence), sequence * 20_000_000,
                            sequence * 20_000_000, ("192.168.1.58", 5005))
            expected = live.snapshot(3_200_000_000)["paths"]
            session = live.stop_recording()
            replay = ReTrackCore(data_dir=root, registry=NodeRegistry(), room="bedroom",
                                 placement="DESK", top_k=4, calibration_file=calibration)
            replay.track_actual = "ON"
            actual = replay_session(replay, session)["paths"]
            self.assertEqual(actual, expected)
            replay.change_placement("CORNER", now_ns=4_000_000_000)
            state = replay.snapshot(4_000_000_001)
            self.assertEqual(state["calibration"]["status"], "REQUIRED")
            self.assertEqual(state["geometry"]["state"], GeometryState.SETTLING.value)

    def test_legacy_ncap_replay_is_immutable_and_uses_capture_placement(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            session = root / "legacy"
            session.mkdir()
            (session / "session.json").write_text(json.dumps({
                "session_id": "legacy-session", "room_id": "bedroom",
                "placement_label": "tonight", "scenario_label": "BACKGROUND",
                "activity_label": "UNLABELED", "occupancy_label": "UNLABELED",
            }), encoding="utf-8")
            archive = ArchiveWriter(session / "frames.ncsi")
            for path in (1, 2, 3):
                archive.append(csi_frame(path, node=2 if path == 2 else 1, path=path),
                               path * 20, path * 30, ("192.168.1.58", 5005))
            archive.close()
            before = (session / "frames.ncsi").read_bytes()
            calibration = root / "calibration.json"
            build_calibration(calibration, placement="TONIGHT_FIXED")
            core = ReTrackCore(data_dir=root, registry=NodeRegistry(), room="UNSPECIFIED",
                               placement="UNSPECIFIED", top_k=4, calibration_file=calibration)
            snapshot = replay_session(core, session)
            self.assertEqual(set(snapshot["paths"]),
                             {"ROUTER_NEWO", "ROUTER_NEWO2", "NEWO2_NEWO"})
            self.assertEqual(snapshot["replay_source"]["format"], "LEGACY_NCAP_V2")
            self.assertEqual(snapshot["replay_source"]["placement"], "tonight")
            self.assertEqual(snapshot["calibration"]["status"], "REJECTED")
            self.assertEqual(snapshot["calibration"]["reason"], "placement mismatch")
            self.assertEqual((session / "frames.ncsi").read_bytes(), before)

    def test_guided_calibration_persists_atomically_and_reloads_normally(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            core = ReTrackCore(data_dir=root, registry=NodeRegistry(), room="bedroom",
                               placement="BED_SIDE", top_k=4, calibration_seconds=5)
            core.track_actual = "ON"
            core.start_calibration(now_ns=0)
            self.assertEqual(core.snapshot(1)["calibration_progress"]["stage"],
                             "BUILDING_SELECTION")
            for sequence in range(1, 22):
                now = sequence * 100_000_000
                core.ingest(csi_frame(sequence), now, now, ("192.168.1.58", 5005))
            core.snapshot(2_100_000_000)
            self.assertEqual(core.geometry.state, GeometryState.BUILDING_BASELINE)
            for sequence in range(22, 56):
                now = sequence * 100_000_000
                core.ingest(csi_frame(sequence), now, now, ("192.168.1.58", 5005))
            snapshot = core.snapshot(5_500_000_000)
            target = root / "calibrations" / "bedroom--BED_SIDE.json"
            self.assertTrue(target.is_file())
            self.assertFalse(target.with_suffix(".json.tmp").exists())
            document = json.loads(target.read_text(encoding="utf-8"))
            self.assertEqual(document["schema_version"], 3)
            self.assertEqual(document["feature_contract"]["feature_schema_version"], 2)
            self.assertTrue(document["calibration_id"])
            self.assertEqual(snapshot["calibration"]["status"], "VALID")
            self.assertEqual(snapshot["geometry"]["state"], "READY")

    def test_guided_calibration_requires_active_measurement_plane(self):
        with tempfile.TemporaryDirectory() as directory:
            core = ReTrackCore(data_dir=Path(directory), registry=NodeRegistry(),
                               room="bedroom", placement="BED_SIDE")
            with self.assertRaisesRegex(RuntimeError, "Track must be ON"):
                core.start_calibration(now_ns=0)
            self.assertEqual(core.geometry.state, GeometryState.CALIBRATION_REQUIRED)
            self.assertFalse(core.calibration_progress(0)["active"])

    def test_interrupted_calibration_never_becomes_ready_and_survives_viewer_detach(self):
        with tempfile.TemporaryDirectory() as directory:
            harness = DaemonHarness(directory)
            host, port = harness.address
            controller = ReTrackClient(host, port, client_id="cal-controller")
            viewer = ReTrackClient(host, port, client_id="cal-viewer")
            try:
                self.assertTrue(controller.acquire_control()["ok"])
                self.assertTrue(controller.mutate("TRACK_SET", state="ON")["ok"])
                result = controller.mutate("CALIBRATION_START", duration_seconds=5)
                self.assertTrue(result["ok"])
                active = ClientBoundaryTests.wait_until(viewer, lambda item: item.get(
                    "calibration_progress", {}).get("active") is True)
                self.assertEqual(active["calibration_progress"]["stage"], "BUILDING_SELECTION")
                controller.close()
                time.sleep(0.03)
                self.assertIsNotNone(harness.daemon.core.pipeline.calibration_stage)
                self.assertFalse(any((Path(directory) / "calibrations").glob("*.json")))
                reconnected = ReTrackClient(host, port, client_id="cal-controller")
                try:
                    self.assertTrue(reconnected.acquire_control(takeover=True)["ok"])
                    self.assertTrue(reconnected.mutate("CALIBRATION_CANCEL")["ok"])
                    cancelled = ClientBoundaryTests.wait_until(viewer, lambda item: not item.get(
                        "calibration_progress", {}).get("active"))
                    self.assertEqual(cancelled["geometry"]["state"], "CALIBRATION_REQUIRED")
                finally:
                    reconnected.close()
            finally:
                viewer.close()
                harness.close()

    def test_teacher_annotation_is_optional_and_separate_from_rf_inference(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            core = ReTrackCore(data_dir=root, registry=NodeRegistry(), room="bedroom")
            self.assertEqual(core.snapshot()["teacher"]["status"], "ABSENT")
            writer = core.start_recording()
            core.add_teacher_observation("PERSON_MOVING", "consented low-rate teacher",
                                         monotonic_ns=writer.origin_ns + 123)
            before_fusion = core.snapshot()["fusion"]
            session = core.stop_recording()
            event = [json.loads(line) for line in (session / "events.jsonl").read_text().splitlines()
                     if 'teacher_observation' in line][0]
            self.assertEqual(event["evidence_type"], "GROUND_TRUTH")
            self.assertFalse(event["rf_inference_overwritten"])
            self.assertEqual(event["elapsed_ns"], 123)
            self.assertEqual(core.snapshot()["fusion"], before_fusion)


class PortOwnershipTests(unittest.TestCase):
    def test_only_one_runtime_can_bind_data_port(self):
        with tempfile.TemporaryDirectory() as directory:
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
            probe.close()
            core = ReTrackCore(data_dir=Path(directory), registry=NodeRegistry(), room="bedroom")
            first = NetworkRuntime(core, LocalTrackClient("127.0.0.1"),
                                   bind="127.0.0.1", data_port=port)
            try:
                other_core = ReTrackCore(data_dir=Path(directory), registry=NodeRegistry(), room="bedroom")
                with self.assertRaises(OSError):
                    NetworkRuntime(other_core, LocalTrackClient("127.0.0.1"),
                                   bind="127.0.0.1", data_port=port)
            finally:
                first.close()


if __name__ == "__main__":
    unittest.main()
