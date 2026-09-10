from __future__ import annotations

from pathlib import Path
from dataclasses import replace
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))

from newo_csi.archive import ArchivedRecord
from newo_csi.evaluate import evaluate_records
from newo_csi.live import LiveState
from newo_csi.protocol import SYNC, SYNC_SIZE, SyncRecord, decode, encode_sync
from newo_csi.sync import SyncAnalyzer


def sync_record(sequence: int, *, leader_session: int = 10,
                follower_session: int = 20, state: int = 2,
                local: int = 1_000_000, offset: int = 25_000,
                drift_milli_ppm: int = 0, accepted: int = 12) -> SyncRecord:
    return SyncRecord(SYNC, SYNC_SIZE, SYNC_SIZE, 0, b"", 2, b"\x22" * 6,
                      1, state, 99, sequence, local, local + offset,
                      offset, offset, drift_milli_ppm, accepted, 0, 0, 1,
                      leader_session, follower_session, sequence, 50, sequence, 0)


class SyncTests(unittest.TestCase):
    def test_phase6_wire_round_trip_and_malformed(self):
        record = sync_record(3)
        wire = encode_sync(record)
        decoded = decode(wire)
        self.assertEqual(decoded.leader_session_id, 10)
        self.assertEqual(decoded.raw_offset_us, 25_000)
        damaged = bytearray(wire); damaged[40] ^= 1
        with self.assertRaisesRegex(Exception, "CRC-32C"):
            decode(damaged)

    def test_fixed_offset_and_affine_drift_alignment(self):
        analyzer = SyncAnalyzer()
        record = sync_record(8, local=2_000_000, offset=30_000,
                             drift_milli_ppm=15_000, accepted=8)
        self.assertTrue(analyzer.add(record, 1_000_000_000))
        fake_csi = type("Csi", (), {"node_id": 2, "timestamp_us": 3_000_000})()
        self.assertAlmostEqual(analyzer.align(fake_csi), 3_030_015.0)

    def test_duplicate_old_session_and_reboots(self):
        analyzer = SyncAnalyzer()
        self.assertTrue(analyzer.add(sync_record(1), 1))
        self.assertFalse(analyzer.add(sync_record(1), 2))
        self.assertTrue(analyzer.add(sync_record(1, follower_session=21, state=1), 3))
        self.assertFalse(analyzer.add(sync_record(99, follower_session=20), 4))
        self.assertTrue(analyzer.add(sync_record(1, leader_session=11,
                                                 follower_session=21, state=1), 5))
        self.assertFalse(analyzer.add(sync_record(100, leader_session=10,
                                                  follower_session=21), 6))

    def test_invalid_state_never_aligns(self):
        analyzer = SyncAnalyzer(); analyzer.add(sync_record(1, state=4), 1)
        fake_csi = type("Csi", (), {"node_id": 2, "timestamp_us": 3_000_000})()
        self.assertIsNone(analyzer.align(fake_csi))
        self.assertFalse(analyzer.status()["cross_node_alignment_available"])

    def test_legacy_sync_placeholder_does_not_enable_alignment(self):
        legacy = replace(sync_record(1), sync_version=0)
        analyzer = SyncAnalyzer()
        self.assertFalse(analyzer.add(legacy, 1))
        self.assertEqual(analyzer.summary()["state"], "UNAVAILABLE")

    def test_live_and_replay_use_same_sync_model(self):
        records = [sync_record(1, state=1, accepted=1), sync_record(2, state=2)]
        wires = [encode_sync(value) for value in records]
        state = LiveState(4, 1.0, "A", 1.0)
        for index, wire in enumerate(wires):
            state.add(decode(wire), 1_000_000_000 + index * 1_000_000_000)
        archived = [ArchivedRecord(1_000_000_000 + index * 1_000_000_000,
                                   2_000_000_000 + index * 1_000_000_000,
                                   "192.0.2.2", 5005, wire)
                    for index, wire in enumerate(wires)]
        replay = evaluate_records(archived, 1.0)
        self.assertEqual(state.sync.summary()["final_offset_us"],
                         replay["sync"]["final_offset_us"])
        self.assertEqual(state.sync.summary()["state"], replay["sync"]["state"])


if __name__ == "__main__":
    unittest.main()
