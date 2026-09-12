from __future__ import annotations

import queue
import socket
import threading
import time
import uuid

from .protocol import decode_message, encode_message


class ReTrackClient:
    """Derived-state JSON-lines client; it never receives raw NCSI datagrams."""

    def __init__(self, host: str = "127.0.0.1", port: int = 8765, *,
                 client_id: str | None = None, timeout: float = 3.0,
                 socket_factory=socket.socket):
        self.client_id = client_id or f"client-{uuid.uuid4().hex[:12]}"
        self.socket = socket_factory(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.settimeout(timeout)
        self.socket.connect((host, port))
        self._file = self.socket.makefile("rb")
        self._send_lock = threading.Lock()
        self._waiters: dict[str, queue.Queue] = {}
        self._waiters_lock = threading.Lock()
        self._snapshot_lock = threading.Lock()
        self._snapshot: dict[str, object] = {}
        self._snapshot_event = threading.Event()
        self.lease_id: str | None = None
        self.lease_ms = 30_000
        self._last_keepalive = 0.0
        self.closed = False
        self._send({"type": "HELLO", "client_id": self.client_id, "subscribe": True})
        hello = decode_message(self._file.readline().rstrip(b"\n"))
        if hello.get("type") != "HELLO_ACK":
            raise ConnectionError(f"ReTrack daemon rejected client: {hello.get('error')}")
        self.socket.settimeout(None)
        self._reader = threading.Thread(target=self._read, name="retrack-client", daemon=True)
        self._reader.start()

    def _send(self, message: dict[str, object]) -> None:
        with self._send_lock:
            self.socket.sendall(encode_message(message))

    def _read(self) -> None:
        try:
            for line in self._file:
                try:
                    message = decode_message(line.rstrip(b"\n"))
                except (ValueError, UnicodeError):
                    continue
                if message.get("type") == "SNAPSHOT" and isinstance(message.get("snapshot"), dict):
                    with self._snapshot_lock:
                        self._snapshot = message["snapshot"]
                    self._snapshot_event.set()
                    continue
                request_id = message.get("request_id")
                if isinstance(request_id, str):
                    with self._waiters_lock:
                        waiter = self._waiters.get(request_id)
                    if waiter is not None:
                        waiter.put(message)
        except OSError:
            pass
        finally:
            self.closed = True
            with self._waiters_lock:
                for waiter in self._waiters.values():
                    waiter.put({"type": "RESULT", "ok": False, "error": "connection_closed"})

    def request(self, kind: str, *, request_id: str | None = None,
                timeout: float = 5.0, **fields: object) -> dict[str, object]:
        identity = request_id or uuid.uuid4().hex
        waiter: queue.Queue = queue.Queue(maxsize=1)
        with self._waiters_lock:
            self._waiters[identity] = waiter
        try:
            self._send({"type": kind, "request_id": identity, **fields})
            response = waiter.get(timeout=timeout)
        except queue.Empty as error:
            raise TimeoutError(f"ReTrack daemon did not answer {kind}") from error
        finally:
            with self._waiters_lock:
                self._waiters.pop(identity, None)
        return response

    def acquire_control(self, *, takeover: bool = False) -> dict[str, object]:
        result = self.request("ACQUIRE_CONTROL", takeover=takeover)
        if result.get("ok"):
            self.lease_id = str(result["lease_id"])
            self.lease_ms = int(result.get("lease_remaining_ms", self.lease_ms))
            self._last_keepalive = time.monotonic()
        return result

    def maintain(self) -> None:
        if self.lease_id is None or self.closed:
            return
        if time.monotonic() - self._last_keepalive < max(1.0, self.lease_ms / 3000):
            return
        result = self.request("KEEPALIVE", lease_id=self.lease_id)
        if not result.get("ok"):
            self.lease_id = None
        self._last_keepalive = time.monotonic()

    def mutate(self, kind: str, **fields: object) -> dict[str, object]:
        if self.lease_id is None:
            return {"type": "RESULT", "ok": False, "error": "viewer_has_no_controller_lease"}
        return self.request(kind, lease_id=self.lease_id, **fields)

    def wait_snapshot(self, timeout: float = 3.0) -> dict[str, object]:
        if not self._snapshot_event.wait(timeout):
            raise TimeoutError("no ReTrack snapshot received")
        return self.snapshot()

    def snapshot(self) -> dict[str, object]:
        with self._snapshot_lock:
            return dict(self._snapshot)

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.socket.close()
