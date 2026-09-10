from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
sys.path.insert(0, str(Path(__file__).parent))

from newo_csi.annotations import append_annotation, load_annotations
from newo_csi.archive import ArchiveWriter
from newo_csi.evaluate import evaluate_archive
from test_dsp import frame


def make_session(root: Path, session_id: str = "test-session") -> Path:
    session = root / session_id
    session.mkdir()
    (session / "session.json").write_text(json.dumps({
        "schema_version": 1, "session_id": session_id,
        "scenario_label": "BACKGROUND", "activity_label": "original",
    }), encoding="utf-8")
    (session / "events.jsonl").write_text("", encoding="utf-8")
    with ArchiveWriter(session / "frames.ncsi") as writer:
        writer.append(frame(sequence=0).raw, 0, 1000, ("127.0.0.1", 5005))
    (session / "summary.json").write_text("{}\n", encoding="utf-8")
    return session


class AnnotationTests(unittest.TestCase):
    def test_multiple_annotations_preserve_history(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            session = make_session(root)
            annotations = root / "annotations"
            first = append_annotation(session, "FIRST", "initial correction",
                                      annotations_dir=annotations)
            second = append_annotation(session, "WALK_PASS", "range correction",
                                       12.5, 18.0, annotations)
            history = load_annotations("test-session", annotations)
            self.assertEqual([row["label"] for row in history], ["FIRST", "WALK_PASS"])
            self.assertNotEqual(first["annotation_id"], second["annotation_id"])
            self.assertEqual(history[1]["scope"], "ELAPSED_RANGE")

    def test_invalid_ranges(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            session = make_session(root)
            for start, end in ((1, None), (None, 2), (-1, 2), (3, 3), (4, 3)):
                with self.assertRaises(ValueError):
                    append_annotation(session, "BAD", None, start, end, root / "annotations")

    def test_missing_session(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "not found"):
                append_annotation(Path(directory) / "missing", "LABEL", None,
                                  annotations_dir=Path(directory) / "annotations")

    def test_evaluator_separates_original_annotation_and_inference(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            session = make_session(root)
            annotations = root / "annotations"
            append_annotation(session, "CORRECTED", "operator correction",
                              annotations_dir=annotations)
            result = evaluate_archive(session, 1, annotations_dir=annotations)
            self.assertEqual(result["original_capture_metadata"]["activity_label"], "original")
            self.assertEqual(result["operator_post_hoc_annotations"][0]["label"], "CORRECTED")
            self.assertIn("paths", result["dsp_inference"])
            self.assertEqual(json.loads((session / "summary.json").read_text()), {})


if __name__ == "__main__":
    unittest.main()
