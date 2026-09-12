from __future__ import annotations

from pathlib import Path
import json
import time

from retrack.storage import iter_session_records


def replay_session(core, session: Path, speed: float = 0.0) -> dict[str, object]:
    metadata_path = session / "manifest.json"
    if not metadata_path.is_file():
        metadata_path = session / "session.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    core.replay_session_id = metadata.get("session_id", session.name)
    core.replay_source = {
        "format": "RETRACK" if metadata_path.name == "manifest.json" else "LEGACY_NCAP_V2",
        "room": metadata.get("room", metadata.get("room_id")),
        "placement": metadata.get("placement", metadata.get("placement_label")),
    }
    source_room = core.replay_source["room"]
    source_placement = core.replay_source["placement"]
    if isinstance(source_room, str) and source_room:
        core.room = source_room
    if isinstance(source_placement, str) and source_placement:
        core.geometry.room = core.room
        core.geometry.placement = source_placement
    if core.calibration_file is not None:
        # Replay uses capture metadata as evidence. A CLI/config placement cannot
        # silently override it merely to make a calibration match.
        core.load_calibration(core.calibration_file)
    previous_ns: int | None = None
    for item in iter_session_records(session):
        if previous_ns is not None and speed > 0:
            delay = (item.host_monotonic_ns - previous_ns) / 1e9 / speed
            if delay > 0:
                time.sleep(delay)
        core.ingest(item.record, item.host_monotonic_ns, item.host_wall_ns,
                    (item.source_ip, item.source_port))
        previous_ns = item.host_monotonic_ns
    return core.snapshot(previous_ns)
