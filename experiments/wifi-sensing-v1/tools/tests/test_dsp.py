from __future__ import annotations

import json
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))

from newo_csi.dsp import CsiPipeline, PathSnapshot, Welford
from newo_csi.live import LiveState, SessionRecorder, TerminalInput
from newo_csi.archive import ArchiveWriter, iter_archive
from newo_csi.discovery import (CollectorAddress, decode_announcement,
                                encode_announcement, select_collector_address)
from newo_csi.field import render_field
from newo_csi.protocol import CsiRecord, crc32c, decode


def frame(*, path=1, node=1, sequence=0, timestamp=0, channel=6,
          receiver=b"\x10\x11\x12\x13\x14\x15",
          source=b"\x20\x21\x22\x23\x24\x25", values=(10, 20, 30, 40),
          pairs=None) -> CsiRecord:
    iq = bytearray()
    pairs = pairs or tuple((value // 2, value) for value in values)
    for imag, real in pairs:
        iq += struct.pack("bb", imag, real)
    length = 88 + len(iq)
    output = bytearray(length)
    struct.pack_into("<4sBBHII", output, 0, b"NCSI", 1, 1, 88, length, 0)
    struct.pack_into("<I6s6sIQBBBBbbBBHHHHHBB", output, 16, node, receiver, source,
                     sequence, timestamp, channel, 0, 0, 1, -55, -91, 0, 3,
                     len(iq), len(iq), len(pairs), 4, path, 0, 1)
    struct.pack_into("<IBBHBBHHH6sH", output, 64, 0, 0, 4, 0, 1, 0, 100, 0, 0,
                     receiver, 0)
    output[88:] = iq
    struct.pack_into("<I", output, 12, crc32c(output))
    result = decode(bytes(output))
    assert isinstance(result, CsiRecord)
    return result


def feed(pipeline: CsiPipeline, value_fn, *, path=1, count=30, intervals=None,
         channel=6) -> None:
    intervals = intervals or [100_000_000] * count
    now = 0
    for index in range(count):
        now += intervals[index]
        base = value_fn(index)
        pipeline.add(frame(path=path, node=1 if path != 2 else 2,
                           sequence=index, timestamp=now // 1000, channel=channel,
                           values=(base, base + 2, base + 4, base + 6)), now)


class DspTests(unittest.TestCase):
    def test_welford_is_stable_for_large_offset(self):
        stats = Welford()
        for value in (1e12 + 1, 1e12 + 2, 1e12 + 3):
            stats.add(value)
        self.assertAlmostEqual(stats.mean, 1e12 + 2)
        self.assertAlmostEqual(stats.variance, 1.0)

    def test_constant_noise_step_and_periodic_features(self):
        powers = {}
        generators = {
            "constant": lambda _: 20,
            "noise": lambda i: 20 + (i % 3 - 1),
            "step": lambda i: 20 if i < 15 else 60,
            "periodic": lambda i: 20 if i % 2 else 60,
        }
        for name, fn in generators.items():
            pipeline = CsiPipeline(top_k=4, window_seconds=10)
            feed(pipeline, fn)
            powers[name] = pipeline.snapshots()[1].window_power
        self.assertAlmostEqual(powers["constant"] or 0, 0.0)
        self.assertGreater(powers["noise"] or 0, powers["constant"] or 0)
        self.assertGreater(powers["step"] or 0, powers["noise"] or 0)
        self.assertGreater(powers["periodic"] or 0, powers["step"] or 0)

    def test_actual_rate_uses_irregular_timestamps(self):
        pipeline = CsiPipeline(window_seconds=10)
        feed(pipeline, lambda _: 20, count=4,
             intervals=[100_000_000, 200_000_000, 400_000_000, 300_000_000])
        self.assertAlmostEqual(pipeline.snapshots()[1].sample_rate_hz, 3 / 0.9)

    def test_missing_time_gap_does_not_create_derivative_spike(self):
        pipeline = CsiPipeline(top_k=4, window_seconds=2)
        feed(pipeline, lambda i: 20 if i < 3 else 80, count=4,
             intervals=[100_000_000, 100_000_000, 100_000_000, 10_000_000_000])
        self.assertIsNone(pipeline.snapshots()[1].window_power)

    def test_paths_and_geometries_never_mix(self):
        pipeline = CsiPipeline(top_k=4)
        feed(pipeline, lambda _: 20, path=1, count=5)
        feed(pipeline, lambda _: 30, path=2, count=6)
        feed(pipeline, lambda _: 40, path=1, count=4, channel=11)
        self.assertEqual(set(pipeline.snapshots()), {1, 2})
        self.assertEqual(len(pipeline.processors), 3)
        self.assertIn("ch6/0", pipeline.snapshots()[1].geometry)

    def test_receiver_sequence_is_not_counted_as_per_path_loss(self):
        pipeline = CsiPipeline()
        pipeline.add(frame(path=1, sequence=0), 100_000_000)
        pipeline.add(frame(path=3, sequence=1), 200_000_000)
        pipeline.add(frame(path=1, sequence=2), 300_000_000)
        self.assertEqual(pipeline.snapshots()[1].sequence_gaps, 0)
        self.assertEqual(pipeline.snapshots()[3].sequence_gaps, 0)

    def test_calibration_is_exact_geometry_and_enables_scores(self):
        pipeline = CsiPipeline(top_k=4, window_seconds=10)
        pipeline.begin_calibration()
        feed(pipeline, lambda i: 20 + (i % 3), count=10)
        pipeline.freeze_calibration_selection()
        feed(pipeline, lambda i: 20 + (i % 2), count=30)
        document = pipeline.calibration_document("DOOR_LEFT")
        self.assertEqual(len(document["paths"]), 1)
        fresh = CsiPipeline(top_k=4, window_seconds=10)
        fresh.load_calibration(document, "DOOR_LEFT")
        feed(fresh, lambda i: 20 if i < 5 else 70, count=12)
        self.assertTrue(fresh.snapshots()[1].calibrated)
        self.assertIsNotNone(fresh.snapshots()[1].motion_score)
        stale = CsiPipeline(top_k=4, window_seconds=10)
        stale.load_calibration(document, "OTHER")
        self.assertEqual(stale.calibration_status(), "REJECTED")
        self.assertEqual(stale.calibration_report()["reason"], "placement mismatch")
        missing = CsiPipeline(top_k=4, window_seconds=10)
        missing.load_calibration(document, None)
        self.assertEqual(missing.calibration_report()["reason"], "placement missing")

    def test_calibration_contract_and_frozen_indices_fail_closed(self):
        pipeline = CsiPipeline(top_k=2, window_seconds=4)
        pipeline.begin_calibration()
        feed(pipeline, lambda i: 20 + (i % 4), count=12)
        pipeline.freeze_calibration_selection()
        selected = pipeline.snapshots()[1].selected_indices
        scales = dict(pipeline.dominant(1).amplitude_scales)
        feed(pipeline, lambda i: 100 if i % 2 else 10, count=20)
        self.assertEqual(pipeline.snapshots()[1].selected_indices, selected)
        self.assertEqual(pipeline.dominant(1).amplitude_scales, scales)
        document = pipeline.calibration_document("P", "R")

        same = CsiPipeline(top_k=2, window_seconds=4)
        same.load_calibration(document, "P", "R")
        feed(same, lambda i: 100 if i % 2 else 5, count=8)
        self.assertEqual(same.snapshots()[1].selected_indices, selected)

        top_k = CsiPipeline(top_k=3, window_seconds=4)
        top_k.load_calibration(document, "P", "R")
        self.assertEqual(top_k.calibration_report()["reason"], "dsp config mismatch: top_k")
        window = CsiPipeline(top_k=2, window_seconds=5)
        window.load_calibration(document, "P", "R")
        self.assertEqual(window.calibration_report()["reason"], "dsp config mismatch: window_seconds")
        room = CsiPipeline(top_k=2, window_seconds=4)
        room.load_calibration(document, "P", "OTHER")
        self.assertEqual(room.calibration_report()["reason"], "room mismatch")
        changed = json.loads(json.dumps(document))
        changed["feature_contract"]["feature_schema_version"] = 999
        version = CsiPipeline(top_k=2, window_seconds=4)
        version.load_calibration(changed, "P", "R")
        self.assertEqual(version.calibration_report()["reason"], "feature schema mismatch")
        legacy = CsiPipeline(top_k=2, window_seconds=4)
        legacy.load_calibration({"schema_version": 2, "placement": "P", "paths": {}}, "P")
        self.assertIn("legacy calibration incompatible", legacy.calibration_report()["reason"])

    def test_phase_changes_are_diagnostic_only(self):
        pipeline = CsiPipeline(top_k=2, window_seconds=2)
        phase_pairs = ((0, 10), (6, 8), (8, 6), (10, 0))
        for index in range(12):
            pair = phase_pairs[index % len(phase_pairs)]
            pipeline.add(frame(sequence=index, pairs=(pair, pair)), index * 100_000_000)
        self.assertAlmostEqual(pipeline.snapshots()[1].window_power or 0.0, 0.0)
        processor = pipeline.dominant(1)
        self.assertGreater(processor.phase_stats[0].variance, 0.0)

    def test_late_geometry_is_excluded_from_calibration(self):
        pipeline = CsiPipeline(top_k=2, window_seconds=4)
        pipeline.begin_calibration()
        feed(pipeline, lambda i: 20 + i % 3, count=12, channel=6)
        pipeline.freeze_calibration_selection()
        feed(pipeline, lambda i: 20 + i % 2, count=12, channel=6)
        feed(pipeline, lambda i: 30 + i % 2, count=12, channel=11)
        document = pipeline.calibration_document("P", "R")
        self.assertEqual(len(document["paths"]), 1)
        self.assertIn("ch6/0", next(iter(document["paths"])))
        self.assertNotIn("ch11/0", " ".join(document["paths"]))

        scoring = CsiPipeline(top_k=2, window_seconds=4)
        scoring.load_calibration(document, "P", "R")
        feed(scoring, lambda _: 20, count=8, channel=6)
        feed(scoring, lambda _: 30, count=8, channel=11)
        report = scoring.calibration_report()
        self.assertEqual(report["status"], "PARTIAL")
        self.assertEqual(report["reason"], "unmatched observed geometries")
        self.assertTrue(any("ch11/0" in value for value in report["mismatched_geometries"]))
        self.assertTrue(scoring.dominant(1).snapshot().calibrated)

    def test_invalid_frozen_feature_data_is_global_rejection(self):
        pipeline = CsiPipeline(top_k=2, window_seconds=4)
        pipeline.begin_calibration()
        feed(pipeline, lambda i: 20 + i % 3, count=12)
        pipeline.freeze_calibration_selection()
        feed(pipeline, lambda i: 20 + i % 2, count=12)
        document = pipeline.calibration_document("P", "R")
        identity = next(iter(document["paths"]))
        document["paths"][identity]["amplitude_scales"] = {"999": 1.0}
        fresh = CsiPipeline(top_k=2, window_seconds=4)
        fresh.load_calibration(document, "P", "R")
        self.assertEqual(fresh.calibration_report()["status"], "REJECTED")
        self.assertEqual(fresh.calibration_report()["reason"],
                         "calibration path feature data invalid")

    def test_uncalibrated_dominant_only_removes_that_path_score(self):
        calibration = CsiPipeline(top_k=2, window_seconds=4)
        calibration.begin_calibration()
        feed(calibration, lambda i: 20 + i % 3, path=1, count=12)
        feed(calibration, lambda i: 30 + i % 3, path=2, count=12)
        calibration.freeze_calibration_selection()
        feed(calibration, lambda i: 20 + i % 2, path=1, count=20)
        feed(calibration, lambda i: 30 + i % 2, path=2, count=20)
        document = calibration.calibration_document("P", "R")

        scoring = CsiPipeline(top_k=2, window_seconds=4)
        scoring.load_calibration(document, "P", "R")
        feed(scoring, lambda i: 20 + i % 2, path=1, count=6, channel=6)
        feed(scoring, lambda i: 60 + i % 2, path=1, count=12, channel=11)
        feed(scoring, lambda i: 30 + i % 2, path=2, count=12, channel=6)
        snapshots = scoring.snapshots()
        self.assertFalse(snapshots[1].calibrated)
        self.assertIsNone(snapshots[1].motion_score)
        self.assertTrue(snapshots[2].calibrated)
        self.assertIsNotNone(snapshots[2].motion_score)
        report = scoring.calibration_report()
        self.assertEqual(report["status"], "PARTIAL")
        self.assertEqual(report["unmatched_dominant_paths"], [1])

    def test_conservative_structural_fusion_matrix(self):
        pipeline = CsiPipeline()

        def snap(path: int, state: str, quality: str = "GOOD") -> PathSnapshot:
            return PathSnapshot(path, str(path), str(path), 10, 10, -50, quality, 4,
                                1, 0, 0, 2.0, state, 1.0, True, (0, 1, 2, 3))

        cases = [
            ([snap(1, "RF_CHANGE"), snap(2, "QUIET"), snap(3, "QUIET")], "LOW_CONFIDENCE"),
            ([snap(1, "RF_CHANGE"), snap(2, "RF_CHANGE"), snap(3, "QUIET")], "RF_CHANGE"),
            ([snap(1, "RF_CHANGE"), snap(2, "RF_CHANGE"), snap(3, "RF_CHANGE")], "RF_CHANGE"),
            ([snap(1, "MOTION_CANDIDATE"), snap(2, "MOTION_CANDIDATE")], "MOTION_CANDIDATE"),
            ([snap(1, "MOTION_CANDIDATE"), snap(2, "QUIET")], "LOW_CONFIDENCE"),
            ([snap(1, "RF_CHANGE", "LOW")], "LOW_CONFIDENCE"),
        ]
        for snapshots, expected in cases:
            with self.subTest(expected=expected, states=[s.motion_state for s in snapshots]):
                with patch.object(pipeline, "snapshots", return_value={s.path_id: s for s in snapshots}):
                    self.assertEqual(pipeline.fused()[0], expected)

    def test_receiver_loss_ui_does_not_double_interleaved_paths(self):
        for width in (40, 60, 80):
            with self.subTest(width=width):
                state = LiveState(4, 2, "P", 1)
                state.pipeline.add(frame(path=1, sequence=0), 100_000_000)
                state.pipeline.add(frame(path=3, sequence=2), 200_000_000)
                state.pipeline.add(frame(path=2, node=2, sequence=0,
                                         receiver=b"\x30\x31\x32\x33\x34\x35"),
                                   300_000_000)
                text = render_field(state, None, None, width=width)
                self.assertEqual(text.count("RX Newo: gaps=1 dup=0"), 1)
                self.assertEqual(text.count("RX Newo2: gaps=0 dup=0"), 1)
                path_lines = [line for line in text.splitlines()
                              if "ROUTER_" in line or "NEWO2_NEWO" in line]
                self.assertFalse(any("gaps=" in line or "LOSS" in line for line in path_lines))

    def test_terminal_input_non_tty_is_safe(self):
        terminal = TerminalInput()
        with patch.object(sys.stdin, "isatty", return_value=False):
            self.assertIs(terminal.__enter__(), terminal)
            terminal.__exit__(None, None, None)
        self.assertIsNone(terminal.fd)

    @patch("newo_csi.live.time.monotonic", return_value=100.0)
    def test_placement_enters_reposition_and_stales_calibration(self, _):
        state = LiveState(4, 2, "CORNER_A", 10)
        state.pipeline.calibration = {"anything": {}}
        state.change_placement("BED_SIDE")
        self.assertTrue(state.repositioning)
        self.assertEqual(state.pipeline.calibration_status(), "REJECTED")
        self.assertIn("placement changed", state.pipeline.calibration_report()["reason"])
        self.assertEqual(state.pipeline.fused(state.repositioning)[0], "REPOSITIONING")

    def test_event_logging_and_exact_archive(self):
        raw = frame().raw
        with tempfile.TemporaryDirectory() as directory:
            recorder = SessionRecorder(Path(directory), "ROOM", "BACKGROUND",
                                       "DOOR_LEFT", "EMPTY", "QUIET")
            recorder.append(raw, 100, 200, ("127.0.0.1", 5005))
            recorder.event("marker", marker="DOOR_OPEN")
            recorder.event("placement_changed", value="BED_SIDE")
            session = recorder.directory
            recorder.close()
            self.assertEqual((session / "frames.ncsi").read_bytes()[-len(raw):], raw)
            events = [json.loads(line) for line in (session / "events.jsonl").read_text().splitlines()]
            self.assertEqual(events[1]["marker"], "DOOR_OPEN")
            self.assertEqual(events[2]["value"], "BED_SIDE")
            metadata = json.loads((session / "session.json").read_text())
            self.assertEqual(metadata["placement_label"], "DOOR_LEFT")

    def test_archive_replay_drives_same_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            archive_path = Path(directory) / "frames.ncsi"
            with ArchiveWriter(archive_path) as writer:
                for index in range(8):
                    writer.append(frame(sequence=index, values=(20 + index,) * 4).raw,
                                  index * 100_000_000, index * 100_000_000,
                                  ("127.0.0.1", 5005))
            pipeline = CsiPipeline(top_k=4)
            direct = CsiPipeline(top_k=4)
            for item in iter_archive(archive_path):
                record = decode(item.record)
                if isinstance(record, CsiRecord):
                    pipeline.add(record, item.host_monotonic_ns)
                    direct.add(record, item.host_monotonic_ns)
            self.assertEqual(pipeline.snapshots()[1].samples, 8)
            self.assertEqual(pipeline.snapshots()[1], direct.snapshots()[1])

    def test_field_renderer_respects_narrow_terminal(self):
        state = LiveState(4, 2, "DOOR_LEFT", 10)
        feed(state.pipeline, lambda i: 20 + i, count=8)
        for width in (32, 40, 60, 80):
            with self.subTest(width=width):
                text = render_field(state, None, None, width=width).replace("\x1b[2J\x1b[H", "")
                self.assertTrue(all(len(line) <= width for line in text.splitlines()))
                self.assertIn("R rec", text)
                self.assertIn("Q quit", text)
                self.assertIn("P:DOOR_LEFT", text)

    def test_collector_announcement_and_address_precedence(self):
        discovered = decode_announcement(encode_announcement(5005, nonce=7), "192.168.1.42")
        self.assertEqual(discovered, CollectorAddress("192.168.1.42", 5005, "announcement"))
        self.assertEqual(select_collector_address(None, discovered,
                                                  ("192.168.1.116", 5005)).source,
                         "announcement")
        selected = select_collector_address(("10.0.0.2", 6000), discovered,
                                            ("192.168.1.116", 5005))
        self.assertEqual((selected.host, selected.port, selected.source),
                         ("10.0.0.2", 6000, "explicit"))


if __name__ == "__main__":
    unittest.main()
