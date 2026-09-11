from __future__ import annotations

import json
from pathlib import Path


def catalog_sessions(root: Path, room: str | None = None) -> list[dict[str, object]]:
    rows = []
    if not root.is_dir():
        return rows
    for path in sorted(root.iterdir(), reverse=True):
        manifest_path = path / "manifest.json"
        if not manifest_path.is_file():
            continue
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if manifest.get("schema") != "retrack_session_v1" or (room and manifest.get("room") != room):
            continue
        rows.append({"session_id": manifest.get("session_id"), "room": manifest.get("room"),
                     "started_at": manifest.get("started_at"), "ended_at": manifest.get("ended_at"),
                     "state": manifest.get("state"), "records": manifest.get("record_count", 0),
                     "placement": manifest.get("placement"), "path": str(path)})
    return rows
