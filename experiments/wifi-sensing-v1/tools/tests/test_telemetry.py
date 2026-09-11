from __future__ import annotations

from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))

from newo_csi.telemetry import SCHEMA, build_track_snapshot


class FakePipeline:
    def snapshots(self):
        return {1: SimpleNamespace(path_name="ROUTER_NEWO", sample_rate_hz=22.5,
            rssi_dbm=-61, signal_quality="GOOD", motion_score=None,
            motion_state="LOW_CONFIDENCE", selected_subcarriers=24, samples=100,
            last_sequence=101, sequence_gaps=2, duplicates=1, geometry="g1")}

    def receiver_losses(self):
        return {(1, "aa:bb:cc:dd:ee:ff"): {"gaps": 2, "duplicates": 1}}

    def calibration_report(self):
        return {"status": "REJECTED", "reason": "geometry mismatch", "placement": "OLD"}

    def fused(self, repositioning):
        return ("REPOSITIONING", 0.0) if repositioning else ("LOW_CONFIDENCE", 0.0)


class FakeSync:
    def summary(self):
        return {"state": "SYNC_WARMING", "sample_count": 4, "accepted_samples": 4,
                "rejected_samples": 0, "final_offset_us": 1200, "drift_ppm": None,
                "last_sync_age_us": 500000, "jitter_us": 100}


class TelemetryTests(unittest.TestCase):
    def state(self):
        return SimpleNamespace(pipeline=FakePipeline(), sync=FakeSync(), repositioning=False,
            placement="DOOR_LEFT", collector_state="REPLAY", records=123, rejected=0,
            node_seen={1: 98.0, 2: 97.0})

    def test_snapshot_is_versioned_bounded_derived_evidence(self):
        value = build_track_snapshot(self.state(), None, room_id="bedroom", collector="PHONE",
                                     now_monotonic=100.0, now_wall_ns=1234)
        self.assertEqual(value["schema"], SCHEMA)
        self.assertEqual(value["path_summary"]["available"], 1)
        self.assertEqual(value["paths"][1], {"path_id": 2, "name": "ROUTER_NEWO2", "available": False})
        self.assertTrue(value["nodes"]["newo"]['online'])
        self.assertEqual(value["sync"]["state"], "SYNC_WARMING")
        self.assertNotIn("raw_csi", value)

    def test_live_and_replay_state_use_identical_snapshot_calculation(self):
        state = self.state()
        live = build_track_snapshot(state, None, room_id="bedroom", collector="DESKTOP",
                                    now_monotonic=100.0, now_wall_ns=1234)
        replay = build_track_snapshot(state, None, room_id="bedroom", collector="DESKTOP",
                                      now_monotonic=100.0, now_wall_ns=1234)
        self.assertEqual(live, replay)

    def test_repositioning_invalidates_geometry_presentation(self):
        state = self.state(); state.repositioning = True
        value = build_track_snapshot(state, None, room_id="bedroom", collector="PHONE",
                                     now_monotonic=100.0, now_wall_ns=1234)
        self.assertEqual(value["geometry_state"], "REPOSITIONING")
        self.assertEqual(value["fusion"]["state"], "REPOSITIONING")


if __name__ == "__main__":
    unittest.main()
