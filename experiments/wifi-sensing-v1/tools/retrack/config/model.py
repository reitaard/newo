from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path


@dataclass
class ReTrackConfig:
    data_dir: Path
    registry_file: Path
    room: str = "UNSPECIFIED"
    placement: str = "UNSPECIFIED"
    bind: str = "0.0.0.0"
    data_port: int = 5005
    control_port: int = 5010
    leader_host: str | None = None
    top_k: int = 24
    window_seconds: float = 2.0
    settle_seconds: float = 10.0
    rotate_bytes: int = 64 * 1024 * 1024
    publisher_enabled: bool = False
    publisher_url: str | None = None


def load_config(path: Path | None = None, *, room: str | None = None) -> ReTrackConfig:
    base = Path.home() / ".config" / "retrack"
    document: dict[str, object] = {}
    if path and path.is_file():
        document = json.loads(path.read_text(encoding="utf-8"))
        if document.get("schema") != "retrack_config_v1":
            raise ValueError("unsupported ReTrack config")
    selected_room = room or str(document.get("default_room", "UNSPECIFIED"))
    rooms = document.get("rooms", {})
    profile = rooms.get(selected_room, {}) if isinstance(rooms, dict) else {}
    defaults = document.get("defaults", {}) if isinstance(document.get("defaults", {}), dict) else {}
    value = lambda name, fallback: profile.get(name, defaults.get(name, fallback))
    data_dir = Path(str(value("data_dir", Path.home() / "retrack-data"))).expanduser()
    return ReTrackConfig(
        data_dir=data_dir, registry_file=Path(str(value("registry_file", base / "nodes.json"))).expanduser(),
        room=selected_room, placement=str(value("placement", "UNSPECIFIED")), bind=str(value("bind", "0.0.0.0")),
        data_port=int(value("data_port", 5005)), control_port=int(value("control_port", 5010)),
        leader_host=value("leader_host", None), top_k=int(value("top_k", 24)),
        window_seconds=float(value("window_seconds", 2.0)), settle_seconds=float(value("settle_seconds", 10.0)),
        rotate_bytes=int(value("rotate_bytes", 64 * 1024 * 1024)),
        publisher_enabled=bool(value("publisher_enabled", False)), publisher_url=value("publisher_url", None),
    )
