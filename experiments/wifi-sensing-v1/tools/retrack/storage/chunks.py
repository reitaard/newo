from __future__ import annotations

from datetime import datetime, timezone
import json
from pathlib import Path
import time
from typing import Iterator
import uuid

from newo_csi.archive import ArchiveError, ArchiveWriter, ArchivedRecord, ENVELOPE, iter_archive


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def atomic_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


class ChunkedSessionWriter:
    """Crash-tolerant chunked NCAP writer; embedded NCSI datagrams stay untouched."""

    def __init__(self, root: Path, *, room: str, nodes: list[dict[str, object]],
                 topology: list[dict[str, object]], placement: str = "UNSPECIFIED",
                 calibration: dict[str, object] | None = None,
                 rotate_bytes: int = 64 * 1024 * 1024, session_id: str | None = None,
                 monotonic_ns=time.monotonic_ns):
        if rotate_bytes < 1024:
            raise ValueError("rotate_bytes must be at least 1024")
        self._clock = monotonic_ns
        self.session_id = session_id or f"{datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}-{uuid.uuid4().hex[:8]}"
        self.directory = root / self.session_id
        self.directory.mkdir(parents=True, exist_ok=False)
        self.events_path = self.directory / "events.jsonl"
        self._events = self.events_path.open("x", encoding="utf-8")
        self._archive: ArchiveWriter | None = None
        self._chunk_bytes = 0
        self._rotate_bytes = rotate_bytes
        self._chunk_index = 0
        self._count = 0
        self._origin_ns = self._clock()
        self.manifest = {
            "schema": "retrack_session_v1", "session_id": self.session_id, "state": "ACTIVE",
            "room": room, "placement": placement, "started_at": utc_now(),
            "monotonic_origin_ns": self._origin_ns, "nodes": nodes, "topology": topology,
            "sync_epochs": [], "calibration": calibration,
            "calibration_version": None if calibration is None else calibration.get("calibration_id"),
            "recording": True,
            "chunks": [], "record_count": 0, "annotations": "events.jsonl",
            "raw_authority": "immutable NCSI datagrams in NCAP v2 envelopes",
        }
        self._open_chunk()
        self._save()
        self.event("recording_started")

    @property
    def origin_ns(self) -> int:
        return self._origin_ns

    def _save(self) -> None:
        self.manifest["record_count"] = self._count
        atomic_json(self.directory / "manifest.json", self.manifest)

    def _open_chunk(self) -> None:
        self._chunk_index += 1
        name = f"frames-{self._chunk_index:06d}.ncsi"
        self._archive = ArchiveWriter(self.directory / name)
        self._chunk_bytes = 0
        self.manifest["chunks"].append({"name": name, "records": 0, "bytes": 0})

    def _rotate(self) -> None:
        assert self._archive is not None
        self._archive.flush()
        self._archive.close()
        self._open_chunk()
        self._save()

    def append(self, raw: bytes, monotonic_ns: int, wall_ns: int, source: tuple[str, int]) -> None:
        size = ENVELOPE.size + len(raw)
        if self._chunk_bytes and self._chunk_bytes + size > self._rotate_bytes:
            self._rotate()
        assert self._archive is not None
        self._archive.append(raw, monotonic_ns, wall_ns, source)
        self._chunk_bytes += size
        self._count += 1
        chunk = self.manifest["chunks"][-1]
        chunk["records"] += 1
        chunk["bytes"] = self._chunk_bytes
        if self._count % 100 == 0:
            self._archive.flush()
            self._save()

    def event(self, label: str, *, monotonic_ns: int | None = None, **fields: object) -> None:
        at_ns = self._clock() if monotonic_ns is None else monotonic_ns
        row = {"schema": "retrack_event_v1", "label": label, "at": utc_now(),
               "host_monotonic_ns": at_ns, "elapsed_ns": max(0, at_ns - self._origin_ns), **fields}
        self._events.write(json.dumps(row, sort_keys=True) + "\n")
        self._events.flush()

    def set_calibration(self, calibration: dict[str, object]) -> None:
        """Atomically associate newly validated calibration without touching raw chunks."""
        self.manifest["calibration"] = calibration
        self.manifest["calibration_version"] = calibration.get("calibration_id")
        self._save()

    def close(self, state: str = "COMPLETE") -> None:
        if self._archive is None:
            return
        self.event("recording_stopped", state=state)
        self._archive.flush()
        self._archive.close()
        self._archive = None
        self._events.close()
        self.manifest.update({"state": state, "recording": False, "ended_at": utc_now()})
        self._save()


def iter_session_records(session: Path) -> Iterator[ArchivedRecord]:
    manifest_path = session / "manifest.json"
    if not manifest_path.is_file():
        legacy = session / "frames.ncsi"
        if legacy.is_file():
            yield from iter_archive(legacy)
            return
        raise FileNotFoundError(f"no ReTrack manifest or legacy frames.ncsi in {session}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for chunk in manifest.get("chunks", []):
        try:
            yield from iter_archive(session / chunk["name"])
        except ArchiveError:
            if chunk is not manifest.get("chunks", [])[-1]:
                raise
            return


def recover_session(session: Path) -> dict[str, object]:
    path = session / "manifest.json"
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("state") == "ACTIVE":
        manifest.update({"state": "INCOMPLETE/RECOVERED", "recording": False,
                         "recovered_at": utc_now()})
        atomic_json(path, manifest)
    return manifest


def recover_active_sessions(root: Path) -> list[Path]:
    """Recover abandoned ACTIVE manifests after the daemon owns the data port."""
    recovered: list[Path] = []
    if not root.is_dir():
        return recovered
    for session in sorted(item for item in root.iterdir() if item.is_dir()):
        manifest_path = session / "manifest.json"
        if not manifest_path.is_file():
            continue
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if manifest.get("state") == "ACTIVE":
            recover_session(session)
            recovered.append(session)
    return recovered
