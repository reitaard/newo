from __future__ import annotations

import tempfile
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))

from newo_csi.archive import ArchiveError, ArchiveWriter, iter_archive
from newo_csi.protocol import (CsiRecord, DiagnosticRecord, ProtocolError,
                               StatusRecord, SyncRecord, crc32c, decode)
from newo_csi.statistics import CaptureStats
from newo_csi.cli import collection_metadata, parser, path_mapping, replay


def csi_frame(*, node: int = 1, path: int = 1, sequence: int = 0,
              receiver: bytes = b"\x10\x11\x12\x13\x14\x15",
              source: bytes = b"\x20\x21\x22\x23\x24\x25",
              iq: bytes = b"\x01\x02\xfd\x04", flags: int = 0x04,
              timestamp: int = 123456) -> bytes:
    length = 88 + len(iq)
    output = bytearray(length)
    struct.pack_into("<4sBBHII", output, 0, b"NCSI", 1, 1, 88, length, 0)
    struct.pack_into("<I6s6sIQBBBBbbBBHHHHHBB", output, 16,
                     node, receiver, source, sequence, timestamp,
                     6, 0, 0, 1, -42, -91, 0, 3,
                     len(iq), len(iq), len(iq) // 2, flags, path, 0, 1)
    struct.pack_into("<IBBHBBHHH6sH", output, 64,
                     0x12345678, 0, 4, 0x8e, 2, 0, 120, 77, 0,
                     receiver, 0)
    output[88:] = iq
    struct.pack_into("<I", output, 12, crc32c(output))
    return bytes(output)


def fixed_frame(record_type: int) -> bytes:
    length = {2: 80, 3: 64, 4: 104}[record_type]
    output = bytearray(length)
    struct.pack_into("<4sBBHII", output, 0, b"NCSI", 1, record_type, length, length, 0)
    if record_type == 2:
        struct.pack_into("<I6sHIIQIIIIIIIIHH", output, 16, 1, b"\x10" * 6,
                         0x1f, 99, 3, 1000, 200, 150, 20, 30, 1, 29, 1, 29, 20, 0)
    elif record_type == 3:
        struct.pack_into("<I6sHIIQQIII", output, 16, 1, b"\x10" * 6,
                         3, 99, 4, 1000, 2000, 50, 29, 1)
    else:
        struct.pack_into("<I6sHIIQ15I", output, 16, 1, b"\x10" * 6,
                         0x1f, 99, 5, 1000,
                         10, 1, 20, 19, 18, 1, 2, 3, 4, 17, 6,
                         101, 102, 103, 7)
    struct.pack_into("<I", output, 12, crc32c(output))
    return bytes(output)


class ProtocolTests(unittest.TestCase):
    def test_valid_frame_and_raw_iq_preservation(self) -> None:
        raw_iq = bytes(range(32))
        wire = csi_frame(iq=raw_iq)
        record = decode(wire)
        self.assertIsInstance(record, CsiRecord)
        self.assertEqual(record.raw, wire)
        self.assertEqual(record.iq_bytes, raw_iq)
        self.assertEqual(record.driver_rx_timestamp_us, 0x12345678)
        self.assertEqual(record.mcs, 4)
        self.assertEqual(record.driver_rx_sequence, 77)
        self.assertEqual(record.destination_mac, record.receiver_mac)

    def test_truncated_frame(self) -> None:
        with self.assertRaisesRegex(ProtocolError, "length"):
            decode(csi_frame()[:-1])

    def test_wrong_magic_and_version(self) -> None:
        for offset, value, message in ((0, ord("X"), "magic"), (4, 2, "version")):
            wire = bytearray(csi_frame())
            wire[offset] = value
            wire[12:16] = b"\0" * 4
            struct.pack_into("<I", wire, 12, crc32c(wire))
            with self.assertRaisesRegex(ProtocolError, message):
                decode(wire)

    def test_status_sync_and_diagnostic_records(self) -> None:
        status = decode(fixed_frame(2))
        sync = decode(fixed_frame(3))
        self.assertIsInstance(status, StatusRecord)
        self.assertEqual(status.transport_drops, 1)
        self.assertIsInstance(sync, SyncRecord)
        self.assertEqual(sync.host_timestamp_us, 2000)
        diagnostic = decode(fixed_frame(4))
        self.assertIsInstance(diagnostic, DiagnosticRecord)
        self.assertEqual(diagnostic.probe_tx_attempted, 20)
        self.assertEqual(diagnostic.probe_tx_submit_failure, 2)
        self.assertEqual(diagnostic.path_3_gate_drops, 103)
        self.assertEqual(diagnostic.association_epoch, 7)


class StatisticsTests(unittest.TestCase):
    def test_sequence_gaps_multiple_nodes_and_paths(self) -> None:
        stats = CaptureStats()
        receiver_two = b"\x30\x31\x32\x33\x34\x35"
        samples = (
            (csi_frame(node=1, path=1, sequence=10), 1_000_000_000),
            (csi_frame(node=2, path=2, sequence=7, receiver=receiver_two), 1_010_000_000),
            (csi_frame(node=1, path=3, sequence=13), 1_050_000_000),
            (csi_frame(node=2, path=2, sequence=8, receiver=receiver_two), 1_060_000_000),
        )
        for wire, timestamp in samples:
            stats.add(decode(wire), timestamp)
        result = stats.as_dict()
        self.assertEqual(result["record_counts"]["csi"], 4)
        self.assertEqual(set(result["paths"]), {"ROUTER_NEWO", "ROUTER_NEWO2", "NEWO2_NEWO"})
        self.assertEqual(sum(result["sequence_gap_estimate_by_receiver"].values()), 2)
        self.assertEqual(result["paths"]["ROUTER_NEWO2"]["frame_count"], 2)

    def test_explicit_three_path_mac_mapping(self) -> None:
        args = SimpleNamespace(router_bssid="aa:bb:cc:dd:ee:ff",
                               newo_mac="10:11:12:13:14:15",
                               newo2_mac="20:21:22:23:24:25")
        mapping = path_mapping(args)
        self.assertEqual(mapping[1], (bytes.fromhex("101112131415"),
                                      bytes.fromhex("aabbccddeeff")))
        self.assertEqual(mapping[2][0], bytes.fromhex("202122232425"))
        self.assertEqual(mapping[3], (bytes.fromhex("101112131415"),
                                      bytes.fromhex("202122232425")))

    def test_diagnostic_sequence_gap_and_host_visible_counters(self) -> None:
        first = bytearray(fixed_frame(4))
        second = bytearray(fixed_frame(4))
        struct.pack_into("<I", first, 32, 10)
        struct.pack_into("<I", second, 32, 13)
        for wire in (first, second):
            wire[12:16] = b"\0" * 4
            struct.pack_into("<I", wire, 12, crc32c(wire))
        stats = CaptureStats()
        stats.add(decode(first), 1_000_000_000)
        stats.add(decode(second), 2_000_000_000)
        result = stats.as_dict()
        self.assertEqual(sum(result["diagnostic_sequence_gap_estimate"].values()), 2)
        latest = result["latest_device_diagnostics"][0]
        self.assertEqual(latest["probe_tx_success"], 18)
        self.assertEqual(latest["path_gate_drops"]["NEWO2_NEWO"], 103)


class MetadataTests(unittest.TestCase):
    def test_collect_placement_is_canonical_and_optional(self) -> None:
        args = parser().parse_args([
            "collect", "--room-id", "ROOM_A", "--scenario", "BACKGROUND",
            "--placement", "DOOR_LEFT",
        ])
        metadata = collection_metadata(args, "session", None, "now", 10, 4096)
        self.assertEqual(metadata["metadata_contract"], "newo_csi_capture_v3")
        self.assertEqual(metadata["placement_label"], "DOOR_LEFT")
        self.assertEqual(metadata["occupancy_label"], metadata["person_label"])
        self.assertIsNone(metadata["path_mapping"])

        omitted = parser().parse_args([
            "collect", "--room-id", "ROOM_A", "--scenario", "BACKGROUND",
        ])
        metadata = collection_metadata(omitted, "old-style", None, "now", 10, 4096)
        self.assertIn("placement_label", metadata)
        self.assertIsNone(metadata["placement_label"])


class ArchiveTests(unittest.TestCase):
    def test_archive_round_trip_preserves_complete_radio_record(self) -> None:
        wire = csi_frame(iq=b"\x80\x7f\x00\xff")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frames.ncsi"
            with ArchiveWriter(path) as writer:
                writer.append(wire, 99, 123456789, ("192.0.2.8", 5005))
            items = list(iter_archive(path))
            self.assertEqual(len(items), 1)
            self.assertEqual(items[0].record, wire)
            self.assertEqual(items[0].host_monotonic_ns, 99)
            self.assertEqual(items[0].host_wall_ns, 123456789)
            self.assertEqual(items[0].source_ip, "192.0.2.8")
            self.assertEqual(items[0].source_port, 5005)

    def test_truncated_archive_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frames.ncsi"
            path.write_bytes(b"NCAP")
            with self.assertRaises(ArchiveError):
                list(iter_archive(path))

    def test_replay_sends_original_datagram_bytes(self) -> None:
        first = csi_frame(sequence=1)
        second = csi_frame(sequence=2, path=3)

        class FakeSocket:
            def __init__(self) -> None:
                self.sent = []
            def sendto(self, data: bytes, destination: tuple[str, int]) -> None:
                self.sent.append((data, destination))
            def close(self) -> None:
                pass

        fake = FakeSocket()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frames.ncsi"
            with ArchiveWriter(path) as writer:
                writer.append(first, 100, 1000, ("192.0.2.1", 5005))
                writer.append(second, 200, 900, ("192.0.2.2", 5005))
            args = SimpleNamespace(session=str(path), host="127.0.0.1", port=6000,
                                   speed=0.0, csi_only=False)
            with patch("newo_csi.cli.socket.socket", return_value=fake):
                self.assertEqual(replay(args), 0)
        self.assertEqual([item[0] for item in fake.sent], [first, second])
        self.assertEqual({item[1] for item in fake.sent}, {("127.0.0.1", 6000)})

    def test_replay_uses_monotonic_delta_despite_wall_clock_jump(self) -> None:
        fake = SimpleNamespace(sent=[], sendto=lambda data, destination: None,
                               close=lambda: None)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frames.ncsi"
            with ArchiveWriter(path) as writer:
                writer.append(csi_frame(sequence=1), 1_000_000_000,
                              9_000_000_000, ("192.0.2.1", 5005))
                writer.append(csi_frame(sequence=2), 1_050_000_000,
                              1_000_000_000, ("192.0.2.1", 5005))
            args = SimpleNamespace(session=str(path), host="127.0.0.1", port=6000,
                                   speed=1.0, csi_only=False)
            with patch("newo_csi.cli.socket.socket", return_value=fake), \
                 patch("newo_csi.cli.time.sleep") as sleep:
                self.assertEqual(replay(args), 0)
        sleep.assert_called_once_with(0.05)


if __name__ == "__main__":
    unittest.main()
