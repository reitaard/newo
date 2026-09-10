"""Small JSON configuration for repeatable desktop/Termux field sessions."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

DEFAULT_FIELD_CONFIG = Path.home() / ".config" / "newo-csi" / "field.json"
FIELD_DEFAULTS: dict[str, Any] = {
    "bind": "0.0.0.0", "port": 5005, "receive_buffer": 4 * 1024 * 1024,
    "dataset_dir": str(Path.home() / "newo-csi-data" / "datasets"),
    "scenario": "BACKGROUND", "placement": "UNSPECIFIED", "top_k": 24,
    "window_seconds": 2.0, "settle_seconds": 10.0,
}


def apply_field_config(args: Any) -> Any:
    path = Path(args.config).expanduser() if args.config else DEFAULT_FIELD_CONFIG
    document: dict[str, Any] = {}
    if path.is_file():
        document = json.loads(path.read_text(encoding="utf-8"))
        if document.get("schema_version") != 1:
            raise ValueError(f"unsupported field config schema: {path}")
    room_name = args.room or args.room_id or document.get("default_room") or "UNSPECIFIED"
    rooms = document.get("rooms", {})
    if not isinstance(rooms, dict):
        raise ValueError("field config rooms must be an object")
    room = rooms.get(room_name, {})
    if not isinstance(room, dict):
        raise ValueError(f"field config room must be an object: {room_name}")
    defaults = document.get("defaults", {})
    if not isinstance(defaults, dict):
        raise ValueError("field config defaults must be an object")
    for name, fallback in FIELD_DEFAULTS.items():
        current = getattr(args, name, None)
        setattr(args, name, current if current is not None else room.get(name, defaults.get(name, fallback)))
    args.room_id = args.room_id or room.get("room_id", room_name)
    args.calibration_file = args.calibration_file or room.get(
        "calibration_file", defaults.get("calibration_file",
        str(Path(args.dataset_dir).expanduser().parent / "calibrations" / f"{args.room_id}-{args.placement}.json")))
    args.dataset_dir = str(Path(args.dataset_dir).expanduser())
    args.calibration_file = str(Path(args.calibration_file).expanduser())
    return args
