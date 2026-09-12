from __future__ import annotations

import socket
import time
import uuid

from .protocol import CONTROL_PORT, ControlAck, ControlCommand


def local_broadcast_targets() -> tuple[str, ...]:
    targets = {"255.255.255.255"}
    try:
        for entry in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            octets = entry[4][0].split(".")
            if len(octets) == 4 and octets[0] not in ("0", "127"):
                targets.add(".".join((*octets[:3], "255")))
    except OSError:
        pass
    return tuple(sorted(targets))


class LocalTrackClient:
    def __init__(self, host: str | None = None, *, port: int = CONTROL_PORT,
                 session_id: str | None = None, lease_ms: int = 15_000,
                 timeout: float = 0.8, socket_factory=socket.socket):
        self.host = host
        self.port = port
        self.session_id = session_id or uuid.uuid4().hex
        self.lease_ms = lease_ms
        self.timeout = timeout
        self.command_id = 0
        self._socket_factory = socket_factory

    def _next(self, command: str, state: str | None = None) -> ControlCommand:
        self.command_id += 1
        return ControlCommand(self.session_id, self.command_id, command, state, self.lease_ms)

    def transact(self, command: ControlCommand, *, attempts: int = 3) -> ControlAck:
        destinations = (self.host,) if self.host else local_broadcast_targets()
        payload = command.encode()
        sock = self._socket_factory(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.settimeout(self.timeout)
            for _ in range(max(1, attempts)):
                for destination in destinations:
                    sock.sendto(payload, (destination, self.port))
                deadline = time.monotonic() + self.timeout
                while time.monotonic() < deadline:
                    try:
                        response, source = sock.recvfrom(1024)
                    except socket.timeout:
                        break
                    try:
                        ack = ControlAck.decode(response)
                    except (ValueError, UnicodeError):
                        continue
                    if ack.session_id == command.session_id and ack.command_id == command.command_id:
                        self.host = source[0]
                        return ack
            raise TimeoutError(f"no ReTrack control ACK on local LAN port {self.port}")
        finally:
            sock.close()

    def status(self) -> ControlAck:
        return self.transact(self._next("STATUS"))

    def set_track(self, enabled: bool) -> ControlAck:
        ack = self.transact(self._next("TRACK_SET", "ON" if enabled else "OFF"))
        if not enabled and ack.accepted and ack.actual == "OFF":
            self.session_id = uuid.uuid4().hex
            self.command_id = 0
        return ack

    def renew(self) -> ControlAck:
        return self.status()
