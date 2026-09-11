from __future__ import annotations

from dataclasses import asdict, dataclass
import json
import time

PROTOCOL = "retrack_control_v1"
CONTROL_PORT = 5010


@dataclass(frozen=True)
class ControlCommand:
    session_id: str
    command_id: int
    command: str
    state: str | None = None
    lease_ms: int = 15_000

    def validate(self) -> None:
        if not self.session_id or len(self.session_id) > 64:
            raise ValueError("session_id must be 1..64 characters")
        if not isinstance(self.command_id, int) or self.command_id < 1:
            raise ValueError("command_id must be positive")
        if self.command not in ("TRACK_SET", "STATUS"):
            raise ValueError("unsupported control command")
        if self.command == "TRACK_SET" and self.state not in ("ON", "OFF"):
            raise ValueError("TRACK_SET requires explicit ON or OFF")
        if not 5_000 <= self.lease_ms <= 60_000:
            raise ValueError("lease_ms must be 5000..60000")

    def encode(self) -> bytes:
        self.validate()
        row = {"protocol": PROTOCOL, "type": self.command, "session_id": self.session_id,
               "command_id": self.command_id, "lease_ms": self.lease_ms}
        if self.state is not None:
            row["state"] = self.state
        return json.dumps(row, separators=(",", ":"), sort_keys=True).encode("utf-8")

    @classmethod
    def decode(cls, payload: bytes) -> "ControlCommand":
        if len(payload) > 512:
            raise ValueError("control frame too large")
        row = json.loads(payload.decode("utf-8"))
        if row.get("protocol") != PROTOCOL:
            raise ValueError("unsupported control protocol")
        value = cls(str(row.get("session_id", "")), row.get("command_id"),
                    str(row.get("type", "")), row.get("state"), row.get("lease_ms", 15_000))
        value.validate()
        return value


@dataclass(frozen=True)
class ControlAck:
    session_id: str
    command_id: int
    accepted: bool
    actual: str
    owner: str
    duplicate: bool = False
    lease_remaining_ms: int = 0
    error: str | None = None
    node_id: str | None = None
    hardware_mac: str | None = None
    firmware: str | None = None
    capabilities: tuple[str, ...] = ()

    @classmethod
    def decode(cls, payload: bytes) -> "ControlAck":
        if len(payload) > 1024:
            raise ValueError("control ACK too large")
        row = json.loads(payload.decode("utf-8"))
        if row.get("protocol") != PROTOCOL or row.get("type") != "ACK":
            raise ValueError("invalid control ACK")
        return cls(str(row.get("session_id", "")), int(row.get("command_id", 0)),
                   row.get("accepted") is True, str(row.get("actual", "UNKNOWN")),
                   str(row.get("owner", "NONE")), row.get("duplicate") is True,
                   int(row.get("lease_remaining_ms", 0)), row.get("error"), row.get("node_id"),
                   row.get("hardware_mac"), row.get("firmware"), tuple(row.get("capabilities", ())))


class LocalOwnership:
    """Deterministic executable model shared by protocol tests and host behavior."""

    def __init__(self, now_ms=lambda: int(time.monotonic() * 1000)):
        self.now_ms = now_ms
        self.actual = "OFF"
        self.session_id: str | None = None
        self.last_command_id = 0
        self.lease_until_ms = 0
        self._last_ack: ControlAck | None = None
        self._closed_session: str | None = None
        self._closed_command_id = 0
        self._closed_ack: ControlAck | None = None

    def expire(self) -> bool:
        if self.session_id is not None and self.now_ms() >= self.lease_until_ms:
            self.actual = "OFF"
            self.session_id = None
            self.last_command_id = 0
            self._last_ack = None
            return True
        return False

    def apply(self, command: ControlCommand) -> ControlAck:
        command.validate()
        self.expire()
        if self.session_id not in (None, command.session_id):
            return ControlAck(command.session_id, command.command_id, False, self.actual, "LOCAL",
                              error="local_session_busy")
        if self.session_id is None and command.session_id == self._closed_session:
            if command.command_id == self._closed_command_id and self._closed_ack:
                return ControlAck(**{**asdict(self._closed_ack), "duplicate": True})
            return ControlAck(command.session_id, command.command_id, False, self.actual, "NONE",
                              error="session_closed")
        if self.session_id == command.session_id and command.command_id < self.last_command_id:
            return ControlAck(command.session_id, command.command_id, False, self.actual, "LOCAL",
                              error="stale_command")
        if self.session_id == command.session_id and command.command_id == self.last_command_id and self._last_ack:
            return ControlAck(**{**asdict(self._last_ack), "duplicate": True})
        if command.command == "TRACK_SET" and command.state == "ON" and self.session_id is None:
            self.session_id = command.session_id
        if command.command == "TRACK_SET":
            self.actual = command.state or self.actual
        owner = "LOCAL" if self.session_id is not None else "NONE"
        if self.session_id == command.session_id:
            self.lease_until_ms = self.now_ms() + command.lease_ms
            self.last_command_id = command.command_id
        if command.command == "TRACK_SET" and command.state == "OFF" and self.session_id == command.session_id:
            self._closed_session = command.session_id
            self._closed_command_id = command.command_id
            self.session_id = None
            self.lease_until_ms = 0
            owner = "NONE"
        ack = ControlAck(command.session_id, command.command_id, True, self.actual, owner,
                         lease_remaining_ms=max(0, self.lease_until_ms - self.now_ms()))
        self._last_ack = ack
        if self._closed_session == command.session_id and command.command_id == self._closed_command_id:
            self._closed_ack = ack
        return ack

    def cloud_allowed(self, action: str) -> bool:
        self.expire()
        return self.session_id is None or action == "status"
