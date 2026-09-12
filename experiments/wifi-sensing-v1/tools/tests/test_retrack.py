from __future__ import annotations

import json
from pathlib import Path
import struct
import tempfile
import unittest

from newo_csi.archive import iter_archive
from newo_csi.protocol import crc32c
from retrack.collector import ReTrackCore
from retrack.config import load_config
from retrack.control import ControlCommand, LocalOwnership
from retrack.control.client import local_broadcast_targets
from retrack.nodes import NodeRegistry
from retrack.replay import replay_session
from retrack.sessions import GeometryState
from retrack.storage import ChunkedSessionWriter, iter_session_records, recover_session
from retrack.topology import Link, Topology
from retrack.ui import render


def csi_frame(sequence: int, *, node: int = 1, path: int = 1,
              receiver: bytes = b"\x10\x11\x12\x13\x14\x15",
              source: bytes = b"\x20\x21\x22\x23\x24\x25") -> bytes:
    iq = bytes((index + sequence) % 127 for index in range(64))
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


class RegistryTests(unittest.TestCase):
    def test_dynamic_registry_four_slots_and_identity_survives_ip_change(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nodes.json"
            registry = NodeRegistry(path)
            first = registry.register("28:84:85:4a:2c:c4", friendly_name="Newo2", last_ip="192.168.1.4")
            second = registry.register("28-84-85-4A-2C-C4", last_ip="192.168.1.44")
            self.assertEqual(first.node_id, second.node_id)
            self.assertEqual(second.last_ip, "192.168.1.44")
            self.assertEqual(len(registry.visible_slots(4)), 4)
            self.assertEqual(sum(item is None for item in registry.visible_slots(4)), 3)
            self.assertEqual(NodeRegistry(path).nodes[0].node_id, first.node_id)


class ControlTests(unittest.TestCase):
    def test_auto_discovery_has_limited_broadcast_fallback(self):
        self.assertIn("255.255.255.255", local_broadcast_targets())

    def test_explicit_wire_contract_round_trip(self):
        command = ControlCommand("session-a", 7, "TRACK_SET", "ON", 15_000)
        self.assertEqual(ControlCommand.decode(command.encode()), command)
        with self.assertRaises(ValueError):
            ControlCommand("x", 1, "TRACK_SET", None).encode()

    def test_duplicate_is_idempotent_and_local_lease_blocks_cloud(self):
        now = [1_000]
        owner = LocalOwnership(lambda: now[0])
        command = ControlCommand("session-a", 1, "TRACK_SET", "ON", 5_000)
        first = owner.apply(command)
        duplicate = owner.apply(command)
        self.assertTrue(first.accepted)
        self.assertTrue(duplicate.duplicate)
        self.assertEqual(owner.actual, "ON")
        self.assertFalse(owner.cloud_allowed("off"))
        self.assertTrue(owner.cloud_allowed("status"))
        stopped = ControlCommand("session-a", 2, "TRACK_SET", "OFF", 5_000)
        self.assertTrue(owner.apply(stopped).accepted)
        self.assertTrue(owner.apply(stopped).duplicate)
        self.assertEqual(owner.apply(ControlCommand("session-a", 3, "TRACK_SET", "ON", 5_000)).error,
                         "session_closed")
        owner.apply(ControlCommand("session-b", 1, "TRACK_SET", "ON", 5_000))
        now[0] = 6_001
        self.assertTrue(owner.expire())
        self.assertEqual(owner.actual, "OFF")
        self.assertTrue(owner.cloud_allowed("on"))


class StorageTests(unittest.TestCase):
    def test_chunk_rotation_preserves_raw_and_event_time(self):
        raw = [csi_frame(index) for index in range(1, 8)]
        with tempfile.TemporaryDirectory() as directory:
            clock = iter((1_000, 1_100, 1_200, 1_300)).__next__
            writer = ChunkedSessionWriter(Path(directory), room="bedroom", nodes=[], topology=[],
                                          rotate_bytes=1024, monotonic_ns=clock)
            for index, payload in enumerate(raw):
                writer.append(payload, 2_000 + index, 3_000 + index, ("192.168.1.8", 5005))
            writer.event("walking", monotonic_ns=2_500)
            session = writer.directory
            writer.close()
            manifest = json.loads((session / "manifest.json").read_text(encoding="utf-8"))
            self.assertGreater(len(manifest["chunks"]), 1)
            self.assertEqual([item.record for item in iter_session_records(session)], raw)
            event = [json.loads(line) for line in (session / "events.jsonl").read_text().splitlines()
                     if '"walking"' in line][0]
            self.assertEqual(event["elapsed_ns"], 1_500)
            self.assertEqual(manifest["state"], "COMPLETE")

    def test_active_manifest_recovers_without_rewriting_raw(self):
        with tempfile.TemporaryDirectory() as directory:
            writer = ChunkedSessionWriter(Path(directory), room="bedroom", nodes=[], topology=[])
            writer.append(csi_frame(1), 10, 20, ("192.168.1.2", 5005))
            session = writer.directory
            raw_path = session / "frames-000001.ncsi"
            writer._archive.flush()
            before = raw_path.read_bytes()
            manifest = recover_session(session)
            self.assertEqual(manifest["state"], "INCOMPLETE/RECOVERED")
            self.assertEqual(raw_path.read_bytes(), before)
            writer._archive.close()
            writer._events.close()


class CoreTests(unittest.TestCase):
    def make_core(self, directory: str, publisher=None):
        return ReTrackCore(data_dir=Path(directory), registry=NodeRegistry(), room="bedroom",
                           rotate_bytes=2048, publisher=publisher)

    def test_zero_internet_recording_and_raw_priority(self):
        with tempfile.TemporaryDirectory() as directory:
            core = self.make_core(directory)
            core.start_recording()
            core.pipeline.add = lambda *_: (_ for _ in ()).throw(RuntimeError("derived failure"))
            payload = csi_frame(1)
            with self.assertRaises(RuntimeError):
                core.ingest(payload, 10, 20, ("192.168.1.2", 5005))
            session = core.stop_recording("INCOMPLETE/RECOVERED")
            self.assertEqual([item.record for item in iter_session_records(session)], [payload])
            self.assertEqual(core.snapshot()["publisher"], "DISABLED")

    def test_live_and_replay_use_same_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            live = self.make_core(directory)
            live.start_recording()
            for index in range(1, 61):
                live.ingest(csi_frame(index), index * 20_000_000, index * 20_000_000,
                            ("192.168.1.2", 5005))
            expected = live.snapshot(1_200_000_000)["paths"]
            session = live.stop_recording()
            replay = self.make_core(directory)
            actual = replay_session(replay, session, 0)["paths"]
            self.assertEqual(actual, expected)
            self.assertEqual(replay.snapshot()["session_id"], session.name)

    def test_disconnect_reconnect_partial_topology_and_placement_invalidation(self):
        with tempfile.TemporaryDirectory() as directory:
            core = self.make_core(directory)
            core.ingest(csi_frame(1), 10, 20, ("192.168.1.2", 5005))
            node_id = core.registry.nodes[0].node_id
            core.ingest(csi_frame(2), 30, 40, ("192.168.1.22", 5005))
            self.assertEqual(core.registry.nodes[0].node_id, node_id)
            self.assertEqual(core.registry.nodes[0].last_ip, "192.168.1.22")
            self.assertEqual(len(core.topology.links), 1)
            core.geometry.ready("cal-1")
            core.change_placement("CORNER_A", now_ns=100)
            self.assertIsNone(core.geometry.calibration_id)
            self.assertEqual(core.geometry.state, GeometryState.REPOSITIONING)

    def test_four_slot_tui_is_narrow_width_safe(self):
        with tempfile.TemporaryDirectory() as directory:
            core = self.make_core(directory)
            body = render(core.snapshot(), width=28)
            self.assertIn("N4 Empty", body)
            self.assertTrue(all(len(line) <= 28 for line in body.replace("\x1b[2J\x1b[H", "").splitlines()))


class ConfigTopologyTests(unittest.TestCase):
    def test_optional_publisher_is_disabled_by_default(self):
        self.assertFalse(load_config().publisher_enabled)

    def test_topology_supports_arbitrary_links(self):
        topology = Topology()
        topology.upsert(Link("peer:n3-n4", "RT-N003", "RT-N004", directed=False))
        topology.upsert(Link("router:n1", "router", "RT-N001"))
        self.assertEqual(len(topology.links), 2)


if __name__ == "__main__":
    unittest.main()
