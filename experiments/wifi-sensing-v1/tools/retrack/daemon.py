from __future__ import annotations

from dataclasses import dataclass, field
import queue
import threading
import time

from retrack.api import ClientBroker, ReTrackApiServer
from retrack.storage import recover_active_sessions


@dataclass
class _PendingAction:
    message: dict[str, object]
    complete: threading.Event = field(default_factory=threading.Event)
    result: dict[str, object] | None = None
    error: Exception | None = None


class ReTrackDaemon:
    """Single owner of ESP control, CSI UDP, DSP, sync, and local recording."""

    def __init__(self, runtime, *, api_bind: str = "127.0.0.1", api_port: int = 8765,
                 controller_lease_ms: int = 30_000, snapshot_interval: float = 0.25,
                 action_timeout: float = 6.0):
        self.runtime = runtime
        self.core = runtime.core
        self.action_timeout = action_timeout
        self.actions: queue.Queue[_PendingAction] = queue.Queue(maxsize=128)
        self.broker = ClientBroker(self.submit, controller_lease_ms=controller_lease_ms)
        self.api = ReTrackApiServer(self.broker, bind=api_bind, port=api_port,
                                    snapshot_interval=snapshot_interval)
        self.closed = False
        self.recovered_sessions: list[str] = []

    def submit(self, message: dict[str, object]) -> dict[str, object]:
        pending = _PendingAction(message)
        try:
            self.actions.put(pending, timeout=0.5)
        except queue.Full as error:
            raise RuntimeError("daemon action queue full") from error
        if not pending.complete.wait(self.action_timeout):
            raise TimeoutError("daemon action timed out")
        if pending.error is not None:
            raise pending.error
        return pending.result or {}

    def _execute(self, message: dict[str, object]) -> dict[str, object]:
        kind = message["type"]
        if kind == "TRACK_SET":
            state = message.get("state")
            if state not in ("ON", "OFF"):
                raise ValueError("TRACK_SET requires state ON or OFF")
            ack = self.runtime.set_track(state == "ON")
            if not ack.accepted or ack.actual != state:
                raise RuntimeError(ack.error or f"leader actual state is {ack.actual}")
        elif kind == "RECORD_SET":
            state = message.get("state")
            if state == "ON":
                self.core.start_recording(
                    str(message["label"])[:128] if message.get("label") else None)
            elif state == "OFF":
                self.core.stop_recording()
            else:
                raise ValueError("RECORD_SET requires state ON or OFF")
        elif kind == "EVENT":
            label = message.get("label")
            if not isinstance(label, str) or not label.strip():
                raise ValueError("EVENT requires a label")
            note = message.get("note")
            self.core.add_event(label.strip()[:128], str(note)[:512] if note else None)
        elif kind == "PLACEMENT_SET":
            placement = message.get("placement")
            if not isinstance(placement, str) or not placement.strip():
                raise ValueError("PLACEMENT_SET requires a placement")
            placement = placement.strip()[:128]
            if placement != self.core.geometry.placement:
                self.core.change_placement(placement)
        else:
            raise ValueError("unsupported daemon action")
        return {
            "track": self.core.track_actual,
            "owner": self.core.track_owner,
            "recording": self.core.recording,
            "session_id": self.core.recorder.session_id if self.core.recorder else None,
            "placement": self.core.geometry.placement,
        }

    def _drain_actions(self) -> None:
        while True:
            try:
                pending = self.actions.get_nowait()
            except queue.Empty:
                return
            try:
                pending.result = self._execute(pending.message)
            except Exception as error:
                pending.error = error
            finally:
                pending.complete.set()

    def start(self, *, discover: bool = True) -> None:
        # NetworkRuntime already owns UDP 5005 before recovery runs, so a
        # second daemon cannot mark a live first daemon's manifest abandoned.
        self.recovered_sessions = [str(path) for path in recover_active_sessions(
            self.core.data_dir / "sessions")]
        if discover:
            try:
                self.runtime.discover_leader()
            except TimeoutError:
                pass
        self.broker.update_snapshot(self.core.snapshot())
        self.api.start()

    def step(self) -> None:
        self._drain_actions()
        self.runtime.poll()
        self._drain_actions()
        self.broker.update_snapshot(self.core.snapshot())

    def run(self) -> None:
        self.start()
        try:
            while not self.closed:
                self.step()
        except KeyboardInterrupt:
            pass
        finally:
            self.close()

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        self.api.close()
        if self.core.recording:
            self.core.stop_recording()
        if self.core.track_actual == "ON":
            try:
                self.runtime.set_track(False)
            except TimeoutError:
                pass
        self.runtime.close()
