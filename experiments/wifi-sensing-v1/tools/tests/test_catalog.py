from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
sys.path.insert(0, str(Path(__file__).parent))

from newo_csi.annotations import append_annotation
from newo_csi.archive import ArchiveWriter
from newo_csi.catalog import build_catalog, calibration_compatibility
from newo_csi.dsp import CsiPipeline
from test_dsp import frame

_MISSING = object()


def session_at(root: Path, placement_marker=_MISSING) -> Path:
    session = root / "catalog-session"
    session.mkdir()
    metadata = {"schema_version": 1, "session_id": session.name,
                "room_id": "ROOM_A", "scenario_label": "BACKGROUND",
                "person_label": "two", "activity_label": "stationary",
                "capture_started_monotonic_ns": 0,
                "capture_ended_monotonic_ns": 1_000_000_000}
    if placement_marker is not _MISSING:
        metadata["placement_label"] = placement_marker
    (session / "session.json").write_text(json.dumps(metadata), encoding="utf-8")
    (session / "events.jsonl").write_text("", encoding="utf-8")
    with ArchiveWriter(session / "frames.ncsi") as writer:
        writer.append(frame(path=1, sequence=0).raw, 0, 1000, ("127.0.0.1", 5005))
        writer.append(frame(path=2, node=2, sequence=0).raw, 1_000_000_000, 1001,
                      ("127.0.0.1", 5005))
    return session


class CatalogTests(unittest.TestCase):
    def test_catalog_inventory_and_annotations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            session = session_at(root, "DOOR_LEFT")
            annotation_dir = root / "annotations"
            append_annotation(session, "CORRECTED", "operator note",
                              annotations_dir=annotation_dir)
            document = build_catalog(root, None, annotation_dir)
            row = document["sessions"][0]
            self.assertEqual(row["frame_count"], 2)
            self.assertEqual(row["paths_missing"], ["NEWO2_NEWO"])
            self.assertEqual(row["operator_post_hoc_annotations"][0]["label"], "CORRECTED")
            self.assertEqual(row["raw_archive"]["status"], "READABLE")

    def test_catalog_marks_pre_placement_sessions(self):
        with tempfile.TemporaryDirectory() as directory:
            row = build_catalog(Path(directory), None)["sessions"]
            self.assertEqual(row, [])
            session_at(Path(directory))
            row = build_catalog(Path(directory), None)["sessions"][0]
            self.assertTrue(row["predates_placement_metadata"])

    def test_catalog_is_json_serializable_and_marks_corrupt_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            session = session_at(root, None)
            (session / "frames.ncsi").write_bytes(b"broken")
            document = build_catalog(root, None)
            json.dumps(document)
            self.assertEqual(document["sessions"][0]["raw_archive"]["status"], "ERROR")

    def test_catalog_rejects_missing_root(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                build_catalog(Path(directory) / "missing", None)

    def test_calibration_reasons(self):
        pipeline = CsiPipeline(top_k=4)
        pipeline.begin_calibration()
        for index in range(6):
            pipeline.add(frame(sequence=index, values=(20 + index,) * 4), index * 100_000_000)
        pipeline.freeze_calibration_selection()
        for index in range(6, 14):
            pipeline.add(frame(sequence=index, values=(20 + index % 2,) * 4), index * 100_000_000)
        calibration = pipeline.calibration_document("DOOR_LEFT", "ROOM_A")
        geometry = {next(iter(calibration["paths"]))}
        self.assertEqual(calibration_compatibility({}, geometry, calibration, 4, 2)["reason"],
                         "placement missing")
        self.assertEqual(calibration_compatibility({"placement_label": "OTHER"}, geometry,
                                                   calibration, 4, 2)["reason"], "placement mismatch")
        self.assertEqual(calibration_compatibility({"placement_label": "DOOR_LEFT",
                                                    "room_id": "ROOM_A"}, geometry,
                                                    calibration, 4, 2)["status"], "MATCH")
        self.assertEqual(calibration_compatibility({"placement_label": "DOOR_LEFT",
                                                    "room_id": "ROOM_A"}, {"other"},
                                                    calibration, 4, 2)["reason"], "geometry mismatch")


if __name__ == "__main__":
    unittest.main()
