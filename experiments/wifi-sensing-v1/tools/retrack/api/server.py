from __future__ import annotations

from collections import OrderedDict
import copy
import socket
import threading
import time
import uuid

from .protocol import MAX_LINE_BYTES, MUTATIONS, decode_message, encode_message


class ClientBroker:
    """Versioned snapshot broker and single-controller lease authority."""

    def __init__(self, executor, *, controller_lease_ms: int = 30_000,
                 clock=time.monotonic, cache_size: int = 1024):
        self.executor = executor
        self.controller_lease_ms = max(5_000, min(120_000, int(controller_lease_ms)))
        self.clock = clock
        self.cache_size = max(32, int(cache_size))
        self.daemon_id = uuid.uuid4().hex
        self._lock = threading.RLock()
        self._clients: set[str] = set()
        self._controller_id: str | None = None
        self._controller_lease_id: str | None = None
        self._controller_deadline = 0.0
        self._snapshot: dict[str, object] = {}
        self._revision = 0
        self._responses: OrderedDict[tuple[str, str], dict[str, object]] = OrderedDict()

    def _expire(self) -> None:
        if self._controller_id is not None and self.clock() >= self._controller_deadline:
            self._controller_id = None
            self._controller_lease_id = None
            self._controller_deadline = 0.0

    def controller_status(self) -> dict[str, object]:
        with self._lock:
            self._expire()
            return {
                "controller_id": self._controller_id,
                "lease_remaining_ms": max(0, int((self._controller_deadline - self.clock()) * 1000)),
                "viewer_count": len(self._clients),
                "daemon_id": self.daemon_id,
            }

    def connect(self, client_id: str) -> None:
        with self._lock:
            self._clients.add(client_id)

    def disconnect(self, client_id: str) -> None:
        with self._lock:
            self._clients.discard(client_id)
            # A disconnect never stops Track/recording and does not implicitly
            # release control. The bounded lease expires unless the client returns.

    def update_snapshot(self, snapshot: dict[str, object]) -> int:
        with self._lock:
            self._expire()
            self._snapshot = copy.deepcopy(snapshot)
            self._revision += 1
            return self._revision

    def snapshot(self) -> tuple[int, dict[str, object]]:
        with self._lock:
            value = copy.deepcopy(self._snapshot)
            value["client_api"] = self.controller_status()
            return self._revision, value

    def _result(self, request_id: str, ok: bool, **fields: object) -> dict[str, object]:
        return {"type": "RESULT", "request_id": request_id, "ok": ok, **fields}

    def _remember(self, key: tuple[str, str], response: dict[str, object]) -> dict[str, object]:
        self._responses[key] = copy.deepcopy(response)
        self._responses.move_to_end(key)
        while len(self._responses) > self.cache_size:
            self._responses.popitem(last=False)
        return response

    def handle(self, client_id: str, message: dict[str, object]) -> dict[str, object]:
        request_id = message.get("request_id")
        kind = message.get("type")
        if not isinstance(request_id, str) or not request_id or len(request_id) > 128:
            return self._result("", False, error="request_id_required")
        if not isinstance(kind, str):
            return self._result(request_id, False, error="type_required")
        key = (client_id, request_id)
        with self._lock:
            self._expire()
            prior = self._responses.get(key)
            if prior is not None:
                duplicate = copy.deepcopy(prior)
                duplicate["duplicate"] = True
                return duplicate

            if kind == "CONTROL_STATUS":
                return self._remember(key, self._result(request_id, True, **self.controller_status()))

            if kind == "ACQUIRE_CONTROL":
                takeover = message.get("takeover") is True
                if self._controller_id not in (None, client_id) and not takeover:
                    return self._remember(key, self._result(
                        request_id, False, error="controller_busy", **self.controller_status()))
                if self._controller_id != client_id or self._controller_lease_id is None:
                    self._controller_lease_id = uuid.uuid4().hex
                self._controller_id = client_id
                self._controller_deadline = self.clock() + self.controller_lease_ms / 1000
                return self._remember(key, self._result(
                    request_id, True, lease_id=self._controller_lease_id,
                    **self.controller_status()))

            if kind == "KEEPALIVE":
                if not self._owns(client_id, message.get("lease_id")):
                    return self._remember(key, self._result(request_id, False, error="not_controller"))
                self._controller_deadline = self.clock() + self.controller_lease_ms / 1000
                return self._remember(key, self._result(
                    request_id, True, lease_id=self._controller_lease_id,
                    **self.controller_status()))

            if kind not in MUTATIONS:
                return self._remember(key, self._result(request_id, False, error="unsupported_operation"))
            if not self._owns(client_id, message.get("lease_id")):
                return self._remember(key, self._result(request_id, False, error="not_controller"))

            # Serialized execution plus the bounded response cache ensures the
            # same client mutation cannot run twice, even after a lost response.
            try:
                outcome = self.executor(copy.deepcopy(message))
                response = self._result(request_id, True, **(outcome or {}))
            except Exception as error:  # returned to the trusted local client only
                response = self._result(request_id, False, error=str(error)[:256])
            return self._remember(key, response)

    def _owns(self, client_id: str, lease_id: object) -> bool:
        self._expire()
        return (self._controller_id == client_id and isinstance(lease_id, str)
                and lease_id == self._controller_lease_id)


class ReTrackApiServer:
    def __init__(self, broker: ClientBroker, *, bind: str = "127.0.0.1", port: int = 8765,
                 snapshot_interval: float = 0.25, socket_factory=socket.socket):
        self.broker = broker
        self.snapshot_interval = max(0.05, float(snapshot_interval))
        self.socket = socket_factory(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind((bind, port))
        self.socket.listen(16)
        self.socket.settimeout(0.2)
        self.address = self.socket.getsockname()
        self.closed = False
        self._threads: list[threading.Thread] = []
        self._accept_thread: threading.Thread | None = None

    def start(self) -> None:
        if self._accept_thread is not None:
            return
        self._accept_thread = threading.Thread(target=self._accept, name="retrack-api", daemon=True)
        self._accept_thread.start()

    def _accept(self) -> None:
        while not self.closed:
            try:
                connection, _address = self.socket.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            thread = threading.Thread(target=self._serve, args=(connection,), daemon=True)
            self._threads.append(thread)
            thread.start()

    @staticmethod
    def _send(connection: socket.socket, message: dict[str, object]) -> None:
        connection.sendall(encode_message(message))

    def _serve(self, connection: socket.socket) -> None:
        client_id: str | None = None
        subscribed = True
        buffer = bytearray()
        last_revision = -1
        next_snapshot = 0.0
        connection.settimeout(0.1)
        try:
            while not self.closed:
                try:
                    block = connection.recv(4096)
                    if not block:
                        return
                    buffer.extend(block)
                    if len(buffer) > MAX_LINE_BYTES:
                        return
                except socket.timeout:
                    pass
                while b"\n" in buffer:
                    line, _, remainder = buffer.partition(b"\n")
                    buffer[:] = remainder
                    try:
                        message = decode_message(bytes(line))
                    except (ValueError, UnicodeError):
                        self._send(connection, {"type": "ERROR", "error": "malformed_message"})
                        continue
                    if client_id is None:
                        proposed = message.get("client_id")
                        if message.get("type") != "HELLO" or not isinstance(proposed, str) or not proposed:
                            self._send(connection, {"type": "ERROR", "error": "hello_required"})
                            return
                        client_id = proposed[:128]
                        subscribed = message.get("subscribe") is not False
                        self.broker.connect(client_id)
                        self._send(connection, {
                            "type": "HELLO_ACK", "client_id": client_id,
                            **self.broker.controller_status(),
                        })
                        continue
                    if message.get("type") == "SUBSCRIBE":
                        subscribed = message.get("enabled") is not False
                        self._send(connection, {
                            "type": "RESULT", "request_id": message.get("request_id", ""),
                            "ok": True, "subscribed": subscribed,
                        })
                    else:
                        self._send(connection, self.broker.handle(client_id, message))
                now = time.monotonic()
                if client_id is not None and subscribed and now >= next_snapshot:
                    revision, snapshot = self.broker.snapshot()
                    if revision != last_revision:
                        self._send(connection, {"type": "SNAPSHOT", "revision": revision,
                                                "snapshot": snapshot})
                        last_revision = revision
                    next_snapshot = now + self.snapshot_interval
        except OSError:
            return
        finally:
            if client_id is not None:
                self.broker.disconnect(client_id)
            try:
                connection.close()
            except OSError:
                pass

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            self.socket.close()
        except OSError:
            pass
        if self._accept_thread is not None:
            self._accept_thread.join(timeout=1.0)
