from __future__ import annotations

import json


PROTOCOL = "retrack_client_v1"
MAX_LINE_BYTES = 16 * 1024
MUTATIONS = {"TRACK_SET", "RECORD_SET", "EVENT", "PLACEMENT_SET"}


def encode_message(message: dict[str, object]) -> bytes:
    value = {"protocol": PROTOCOL, **message}
    payload = json.dumps(value, separators=(",", ":"), sort_keys=True).encode("utf-8") + b"\n"
    if len(payload) > MAX_LINE_BYTES:
        raise ValueError("client message exceeds bounded JSON-line size")
    return payload


def decode_message(payload: bytes) -> dict[str, object]:
    if len(payload) > MAX_LINE_BYTES:
        raise ValueError("client message exceeds bounded JSON-line size")
    value = json.loads(payload.decode("utf-8"))
    if not isinstance(value, dict) or value.get("protocol") != PROTOCOL:
        raise ValueError("unsupported ReTrack client protocol")
    return value
