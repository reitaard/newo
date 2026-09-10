"""Append-only operator annotations stored outside immutable session archives."""

from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import re
import uuid
from typing import Any

DEFAULT_ANNOTATIONS_DIR = Path(__file__).resolve().parents[1] / "operator-annotations"


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def session_id_from_path(session: Path) -> str:
    if not session.is_dir() or not (session / "session.json").is_file() or not (session / "frames.ncsi").is_file():
        raise ValueError(f"session archive not found: {session}")
    metadata = json.loads((session / "session.json").read_text(encoding="utf-8"))
    session_id = metadata.get("session_id")
    if not isinstance(session_id, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", session_id):
        raise ValueError("session metadata has invalid session_id")
    return session_id


def validate_range(start: float | None, end: float | None) -> None:
    if (start is None) != (end is None):
        raise ValueError("--start and --end must be supplied together")
    if start is not None and (start < 0 or end is None or end <= start):
        raise ValueError("annotation range requires 0 <= start < end")


def append_annotation(session: Path, label: str, note: str | None,
                      start: float | None = None, end: float | None = None,
                      annotations_dir: Path = DEFAULT_ANNOTATIONS_DIR) -> dict[str, Any]:
    session_id = session_id_from_path(session)
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", label):
        raise ValueError("label must contain only letters, digits, dot, underscore, or hyphen")
    validate_range(start, end)
    annotations_dir.mkdir(parents=True, exist_ok=True)
    record = {
        "schema_version": 1, "annotation_id": str(uuid.uuid4()),
        "session_id": session_id, "created_at": _utc_now(),
        "scope": "SESSION" if start is None else "ELAPSED_RANGE",
        "start_elapsed_seconds": start, "end_elapsed_seconds": end,
        "label": label, "note": note,
    }
    with (annotations_dir / f"{session_id}.jsonl").open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(record, sort_keys=True) + "\n")
        stream.flush()
    return record


def load_annotations(session_id: str,
                     annotations_dir: Path = DEFAULT_ANNOTATIONS_DIR) -> list[dict[str, Any]]:
    path = annotations_dir / f"{session_id}.jsonl"
    if not path.is_file():
        return []
    records = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            record = json.loads(line)
            if record.get("session_id") != session_id:
                raise ValueError(f"annotation/session mismatch in {path}")
            records.append(record)
    return records
