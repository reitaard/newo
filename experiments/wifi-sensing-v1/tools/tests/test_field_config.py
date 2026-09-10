from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))

from newo_csi.config import apply_field_config


class FieldConfigTests(unittest.TestCase):
    def test_named_room_and_explicit_override(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / "field.json"
            config.write_text(json.dumps({
                "schema_version": 1,
                "defaults": {"dataset_dir": str(root / "data"), "port": 5006},
                "rooms": {"bedroom": {"placement": "BED_SIDE", "settle_seconds": 15}},
            }), encoding="utf-8")
            args = SimpleNamespace(config=str(config), room="bedroom", room_id=None,
                                   bind=None, port=6000, receive_buffer=None,
                                   dataset_dir=None, scenario=None, placement=None,
                                   top_k=None, window_seconds=None, settle_seconds=None,
                                   calibration_file=None)
            apply_field_config(args)
            self.assertEqual(args.room_id, "bedroom")
            self.assertEqual(args.placement, "BED_SIDE")
            self.assertEqual(args.port, 6000)
            self.assertEqual(args.scenario, "BACKGROUND")
            self.assertTrue(args.calibration_file.endswith("bedroom-BED_SIDE.json"))

    def test_invalid_schema_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "field.json"
            config.write_text('{"schema_version": 2}', encoding="utf-8")
            args = SimpleNamespace(config=str(config), room=None, room_id=None)
            with self.assertRaises(ValueError):
                apply_field_config(args)


if __name__ == "__main__":
    unittest.main()
