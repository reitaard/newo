from __future__ import annotations

import socket
import time

from newo_csi.discovery import CollectorAnnouncer


class NetworkRuntime:
    """Local-LAN UDP data plane and explicit leader control; no WAN dependency."""

    def __init__(self, core, control, *, bind: str = "0.0.0.0", data_port: int = 5005,
                 receive_buffer: int = 4 * 1024 * 1024, socket_factory=socket.socket):
        self.core = core
        self.control = control
        self.socket = socket_factory(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        self.socket.bind((bind, data_port))
        self.socket.settimeout(0.05)
        self.announcer = CollectorAnnouncer(data_port)
        self._last_renew = 0.0
        self.closed = False

    def discover_leader(self):
        ack = self.control.status()
        if ack.hardware_mac:
            self.core.registry.register(ack.hardware_mac, friendly_name=ack.node_id or "Newo",
                                        role="LEADER", capabilities=ack.capabilities,
                                        last_ip=self.control.host, firmware=ack.firmware,
                                        room=self.core.room, sync_role="LEADER")
        self.core.track_actual = ack.actual
        self.core.track_owner = ack.owner
        return ack

    def set_track(self, enabled: bool):
        ack = self.control.set_track(enabled)
        self.core.track_actual = ack.actual
        self.core.track_owner = ack.owner
        self._last_renew = time.monotonic()
        return ack

    def poll(self):
        now = time.monotonic()
        self.announcer.poll(now)
        if self.core.track_actual == "ON" and now - self._last_renew >= self.control.lease_ms / 3000:
            try:
                ack = self.control.renew()
                self.core.track_actual, self.core.track_owner = ack.actual, ack.owner
            except TimeoutError:
                self.core.track_actual = "UNKNOWN"
            self._last_renew = now
        try:
            raw, source = self.socket.recvfrom(4097)
        except socket.timeout:
            return None
        return self.core.ingest(raw, time.monotonic_ns(), time.time_ns(), source)

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        self.announcer.close()
        self.socket.close()
