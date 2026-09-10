from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))

from newo_csi.report import build_derived_report, write_report_artifacts


class DerivedReportTests(unittest.TestCase):
    def test_small_report_keeps_evidence_boundaries(self):
        evaluation = {
            "original_capture_metadata": {"session_id": "session-1", "room_id": "bedroom",
                                          "placement_label": "BED_SIDE", "firmware_version": "dev"},
            "operator_post_hoc_annotations": [{"label": "WALK"}],
            "dsp_inference": {
                "duration_seconds": 4.0,
                "paths": {"ROUTER_NEWO": {"sample_count": 20, "effective_sample_rate_hz": 5.0,
                          "signal_quality_distribution": {"GOOD": 2}, "csi_geometry_switch_count": 0,
                          "dominant_csi_geometry": {"identity": "g1"}, "state_fraction": {"QUIET": .5}}},
                "windows": [{"elapsed_seconds": 2.0, "window_duration_seconds": 2.0,
                             "included_in_aggregate": True, "fusion_confidence": .6,
                             "paths": {"ROUTER_NEWO": {"state": "RF_CHANGE"}}}],
                "transport": {"record_counts": {"csi": 20}},
                "calibration": {"status": "VALID"}, "nuisance_patterns": {},
                "sync": {"state": "SYNC_VALID", "sample_count": 9},
                "interpretation_boundary": "RF change only; no person inference",
            },
        }
        report = build_derived_report(evaluation)
        self.assertEqual(report["sync"]["state"], "SYNC_VALID")
        self.assertEqual(report["operator_annotations"], [{"label": "WALK"}])
        self.assertEqual(report["ranked_rf_events"][0]["paths"], ["ROUTER_NEWO"])
        self.assertIn("no person", report["interpretation_boundary"])
        with tempfile.TemporaryDirectory() as directory:
            paths = write_report_artifacts(report, Path(directory))
            self.assertEqual({path.suffix for path in paths}, {".json", ".csv", ".md"})
            stored = json.loads(paths[0].read_text(encoding="utf-8"))
            self.assertEqual(stored["contract"], "newo_csi_derived_report_v1")


if __name__ == "__main__":
    unittest.main()
