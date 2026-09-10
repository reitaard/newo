from __future__ import annotations

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
sys.path.insert(0, str(Path(__file__).parent))

from newo_csi.archive import ArchivedRecord, ArchiveWriter
from newo_csi.cli import evaluate_command
from newo_csi.evaluate import (agreement_runs, evaluate_records, longest_run,
                               percentile, single_path_patterns)
from test_dsp import frame


def archived(record, ns):
    return ArchivedRecord(ns, 1_700_000_000_000_000_000 + ns,
                          "127.0.0.1", 5005, record.raw)


def window(*states):
    names = ("ROUTER_NEWO", "ROUTER_NEWO2", "NEWO2_NEWO")
    return {"paths": {name: {"state": state} for name, state in zip(names, states)}}


class EvaluationMathTests(unittest.TestCase):
    def test_percentiles(self):
        values = [1, 2, 3, 4, 5]
        self.assertEqual(percentile(values, .5), 3)
        self.assertEqual(percentile(values, .9), 4.6)
        self.assertIsNone(percentile([], .95))

    def test_state_duration_and_longest_run(self):
        windows = [window("RF_CHANGE", "QUIET", "QUIET"),
                   window("RF_CHANGE", "QUIET", "QUIET"),
                   window("QUIET", "QUIET", "QUIET")]
        self.assertEqual(longest_run(windows, "ROUTER_NEWO", "RF_CHANGE", 2), 4)

    def test_isolated_and_multi_path_agreement(self):
        windows = [window("RF_CHANGE", "QUIET", "QUIET"),
                   window("QUIET", "QUIET", "QUIET"),
                   window("RF_CHANGE", "MOTION_CANDIDATE", "QUIET"),
                   window("RF_CHANGE", "MOTION_CANDIDATE", "RF_CHANGE")]
        transient, sustained = single_path_patterns(windows, 1)
        self.assertEqual(transient, {"count": 1, "duration_seconds": 1})
        self.assertEqual(sustained, {"count": 0, "duration_seconds": 0})
        self.assertEqual(agreement_runs(windows, 2, 1)["duration_seconds"], 1)
        self.assertEqual(agreement_runs(windows, 3, 1)["duration_seconds"], 1)


class EvaluationArchiveTests(unittest.TestCase):
    def test_irregular_timestamps_packet_gap_and_geometry_change(self):
        items = [archived(frame(sequence=0, channel=6), 0),
                 archived(frame(sequence=1, channel=6), 100_000_000),
                 archived(frame(sequence=4, channel=11), 900_000_000),
                 archived(frame(sequence=5, channel=11), 2_000_000_000)]
        result = evaluate_records(items, 1)
        path = result["paths"]["ROUTER_NEWO"]
        self.assertAlmostEqual(path["effective_sample_rate_hz"], 1.5)
        self.assertEqual(path["geometry_transition_count"], 1)
        gaps = result["transport"]["sequence_gap_estimate_by_receiver"]
        self.assertEqual(sum(gaps.values()), 2)
        self.assertGreaterEqual(len(result["windows"]), 2)

    def test_empty_and_short_sessions(self):
        self.assertEqual(evaluate_records([], 1)["windows"], [])
        result = evaluate_records([archived(frame(sequence=0), 100)], 1)
        path = result["paths"]["ROUTER_NEWO"]
        self.assertEqual(path["sample_count"], 1)
        self.assertEqual(path["effective_sample_rate_hz"], 0)
        self.assertIsNone(path["motion_score"]["median"])
        self.assertEqual(path["state_fraction"]["LOW_CONFIDENCE"], 0.0)
        self.assertEqual(result["partial_window_count"], 1)
        self.assertFalse(result["windows"][0]["included_in_aggregate"])

    def test_reposition_event_suspends_window_interpretation(self):
        result = evaluate_records([archived(frame(sequence=0), 0),
                                   archived(frame(sequence=1), 900_000_000),
                                   archived(frame(sequence=2), 1_100_000_000)],
                                  1, reposition_ranges=[(0, 2_000_000_000)])
        path = result["paths"]["ROUTER_NEWO"]
        self.assertEqual(path["state_fraction"]["REPOSITIONING"], 1.0)

    def test_json_command_and_ground_truth_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory) / "session"
            session.mkdir()
            (session / "session.json").write_text(json.dumps({
                "scenario_label": "SPEAKER_MUSIC", "person_label": None,
                "activity_label": "UNLABELED", "placement_label": "BED_SIDE",
            }), encoding="utf-8")
            (session / "events.jsonl").write_text(
                json.dumps({"event": "marker", "marker": "CUSTOM", "note": "speaker started"}) + "\n",
                encoding="utf-8")
            with ArchiveWriter(session / "frames.ncsi") as writer:
                writer.append(frame(sequence=0).raw, 0, 1_700_000_000_000_000_000,
                              ("127.0.0.1", 5005))
            args = SimpleNamespace(datasets=[str(session)], window_seconds=1.0,
                                   calibration_file=None, annotations_dir=str(Path(directory) / "annotations"),
                                   json=True)
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(evaluate_command(args), 0)
            document = json.loads(output.getvalue())
            evaluated = document["datasets"][0]
            self.assertEqual(evaluated["original_capture_metadata"]["scenario_label"], "SPEAKER_MUSIC")
            self.assertIn("independent evidence", evaluated["separation_boundary"])
            self.assertEqual(evaluated["original_capture_events"][0]["note"], "speaker started")
            self.assertIn("paths", evaluated["dsp_inference"])


if __name__ == "__main__":
    unittest.main()
