"""Canonical, backward-compatible capture metadata contract."""

from __future__ import annotations

from typing import Any

CAPTURE_METADATA_SCHEMA_VERSION = 3


def capture_metadata(*, session_id: str, room_id: str, scenario: str,
                     placement: str | None, occupancy: str | None,
                     activity: str | None, started_at: str,
                     started_monotonic_ns: int, zone: str | None = None,
                     camera_frame_id: str | None = None, notes: str | None = None,
                     path_mapping: dict[str, object] | None = None,
                     receive_buffer_bytes: int | None = None) -> dict[str, Any]:
    """Return the one metadata shape used by collect/live/field.

    ``person_label`` is retained as a compatibility alias for old readers;
    ``occupancy_label`` is the canonical Phase-5 operator field.
    """
    return {
        "schema_version": CAPTURE_METADATA_SCHEMA_VERSION,
        "metadata_contract": "newo_csi_capture_v3",
        "session_id": session_id,
        "room_id": room_id,
        "scenario_label": scenario,
        "placement_label": placement,
        "occupancy_label": occupancy,
        "person_label": occupancy,
        "activity_label": activity,
        "zone_label": zone,
        "camera_frame_id": camera_frame_id,
        "notes": notes,
        "path_mapping": path_mapping,
        "capture_started_at": started_at,
        "capture_started_monotonic_ns": started_monotonic_ns,
        "udp_receive_buffer_bytes": receive_buffer_bytes,
    }
