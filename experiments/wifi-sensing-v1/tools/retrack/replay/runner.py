from __future__ import annotations

from pathlib import Path
import time

from retrack.storage import iter_session_records


def replay_session(core, session: Path, speed: float = 0.0) -> dict[str, object]:
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
