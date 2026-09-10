"""ANSI research console for live UDP capture or deterministic archive replay."""

from __future__ import annotations

import json
from contextlib import contextmanager, nullcontext
from pathlib import Path
import select
import socket
import sys
import time
from typing import Callable, Iterator

from .archive import ArchiveWriter, ArchivedRecord, iter_archive
from .dsp import CsiPipeline
from .metadata import capture_metadata
from .protocol import CsiRecord, DiagnosticRecord, PATH_NAMES, ProtocolError, Record, StatusRecord, decode
from .statistics import CaptureStats

EVENT_TYPES = ("DOOR_OPEN", "DOOR_CLOSE", "ENTER", "EXIT", "WALK", "WAVE", "CUSTOM")


def utc_now() -> str:
    from datetime import datetime, timezone
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def atomic_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


class SessionRecorder:
    def __init__(self, dataset_dir: Path, room_id: str, scenario: str,
                 placement: str, occupancy: str, activity: str):
        from .cli import session_identifier
        self.session_id = session_identifier(None, scenario)
        self.directory = dataset_dir / self.session_id
        self.directory.mkdir(parents=True, exist_ok=False)
        self.metadata = capture_metadata(
            session_id=self.session_id, room_id=room_id, scenario=scenario,
            placement=placement, occupancy=occupancy, activity=activity,
            started_at=utc_now(), started_monotonic_ns=time.monotonic_ns())
        self.metadata["interpretation"] = "research labels; no person detection or localization claim"
        self.started_monotonic = time.monotonic()
        atomic_json(self.directory / "session.json", self.metadata)
        self.events = (self.directory / "events.jsonl").open("x", encoding="utf-8")
        self.archive = ArchiveWriter(self.directory / "frames.ncsi")
        self.count = 0
        self.stats = CaptureStats()
        self.event("capture_started", scenario=scenario, placement=placement,
                   occupancy=occupancy, activity=activity)

    def event(self, event: str, **fields: object) -> None:
        row = {"event": event, "at": utc_now(), "host_monotonic_ns": time.monotonic_ns(), **fields}
        self.events.write(json.dumps(row, sort_keys=True) + "\n")
        self.events.flush()

    def append(self, raw: bytes, monotonic_ns: int, wall_ns: int,
               source: tuple[str, int], record: Record | None = None) -> None:
        self.archive.append(raw, monotonic_ns, wall_ns, source)
        self.stats.add(record if record is not None else decode(raw), monotonic_ns)
        self.count += 1
        if self.count % 100 == 0:
            self.archive.flush()

    def close(self, reason: str = "user") -> None:
        self.event("capture_ended", reason=reason, records=self.count)
        self.archive.close()
        self.events.close()
        self.metadata["capture_ended_at"] = utc_now()
        self.metadata["capture_ended_monotonic_ns"] = time.monotonic_ns()
        self.metadata["record_count"] = self.count
        atomic_json(self.directory / "session.json", self.metadata)
        summary = self.stats.as_dict()
        summary.update({"schema_version": 1, "session_id": self.session_id})
        atomic_json(self.directory / "summary.json", summary)


class LiveState:
    def __init__(self, top_k: int, window_seconds: float, placement: str,
                 settle_seconds: float):
        self.pipeline = CsiPipeline(top_k, window_seconds)
        self.placement = placement
        self.occupancy = "UNLABELED"
        self.activity = "UNLABELED"
        self.settle_seconds = settle_seconds
        self.reposition_until = 0.0
        self.records = 0
        self.rejected = 0
        self.last_udp_monotonic = 0.0
        self.node_seen: dict[int, float] = {}
        self.channels: set[int] = set()
        self.latest_status: dict[int, StatusRecord] = {}
        self.latest_diagnostic: dict[int, DiagnosticRecord] = {}

    @property
    def repositioning(self) -> bool:
        return time.monotonic() < self.reposition_until

    def change_placement(self, value: str) -> None:
        self.placement = value.strip() or "UNSPECIFIED"
        self.reposition_until = time.monotonic() + self.settle_seconds
        self.pipeline.calibration = {}
        self.pipeline.calibration_rejection = "placement changed; recalibration required"
        for processor in self.pipeline.processors.values():
            processor.baseline = None

    def add(self, record: Record, host_ns: int) -> None:
        self.records += 1
        self.last_udp_monotonic = time.monotonic()
        node = getattr(record, "node_id", None)
        if node is not None:
            self.node_seen[node] = self.last_udp_monotonic
        if isinstance(record, CsiRecord):
            self.channels.add(record.channel)
            if not self.repositioning:
                self.pipeline.add(record, host_ns)
        elif isinstance(record, StatusRecord):
            self.latest_status[record.node_id] = record
        elif isinstance(record, DiagnosticRecord):
            self.latest_diagnostic[record.node_id] = record


class TerminalInput:
    """Nonblocking single-key input with guaranteed POSIX terminal restoration."""

    def __init__(self) -> None:
        self.fd: int | None = None
        self.saved: object | None = None

    def __enter__(self) -> "TerminalInput":
        if sys.platform != "win32" and sys.stdin.isatty():
            import termios
            import tty
            self.fd = sys.stdin.fileno()
            self.saved = termios.tcgetattr(self.fd)
            try:
                tty.setcbreak(self.fd)
            except BaseException:
                self.restore()
                raise
        return self

    def __exit__(self, *_: object) -> None:
        self.restore()

    def restore(self) -> None:
        if self.fd is not None and self.saved is not None:
            import termios
            termios.tcsetattr(self.fd, termios.TCSADRAIN, self.saved)
            self.fd = None
            self.saved = None

    def read_key(self) -> str | None:
        if sys.platform == "win32":
            import msvcrt
            return msvcrt.getwch() if msvcrt.kbhit() else None
        readable, _, _ = select.select([sys.stdin], [], [], 0)
        return sys.stdin.read(1) if readable else None

    @contextmanager
    def cooked(self):
        was_active = self.fd is not None
        self.restore()
        try:
            yield
        finally:
            if was_active:
                self.__enter__()


def prompt(label: str, default: str = "", terminal: TerminalInput | None = None) -> str:
    context = terminal.cooked() if terminal else nullcontext()
    with context:
        sys.stdout.write(f"\x1b[0m\x1b[2J\x1b[H{label} [{default}]: ")
        sys.stdout.flush()
        value = input().strip()
    return value or default


def render(state: LiveState, recorder: SessionRecorder | None,
           calibration_left: float | None) -> str:
    now = time.monotonic()
    online = lambda node: "ONLINE" if now - state.node_seen.get(node, -1e9) < 5 else "OFFLINE"
    udp = "OK" if now - state.last_udp_monotonic < 3 else "NO DATA"
    room_state, confidence = state.pipeline.fused(state.repositioning)
    calibration = state.pipeline.calibration_report()
    lines = ["Newo CSI Phase 5 research console",
             f"Newo: {online(1):7}  Newo2: {online(2):7}  UDP: {udp:7}  channel: {','.join(map(str, sorted(state.channels))) or '-'}",
             f"placement={state.placement} occupancy={state.occupancy} activity={state.activity}",
             f"room={room_state} confidence={confidence:.2f} presence=UNSUPPORTED calibration={calibration['status']}",
             f"calibration_reason={calibration['reason'] or '-'} age_s={calibration['age_seconds']}",
             f"recording={'ON ' + recorder.session_id if recorder else 'OFF'} records={state.records} rejected={state.rejected}"]
    if calibration_left is not None:
        lines.append(f"CALIBRATING quiet/empty baseline: {max(0.0, calibration_left):.1f}s remaining")
    lines += ["", "PATH             Hz    RSSI  geometry                         sequence        score/state              quality  K"]
    snapshots = state.pipeline.snapshots()
    for path_id in (1, 2, 3):
        value = snapshots.get(path_id)
        if value is None:
            lines.append(f"{PATH_NAMES[path_id]:16} --     --    --                               --/--           LOW_CONFIDENCE           --      0")
            continue
        geometry = value.geometry.split(":")[-1]
        score = "--" if value.motion_score is None else f"{value.motion_score:.2f}"
        lines.append(f"{value.path_name:16} {value.sample_rate_hz:5.1f}  {str(value.rssi_dbm):>4}  {geometry[:30]:30} "
                     f"{str(value.last_sequence):>8}       {score:>5}/{value.motion_state:16} {value.signal_quality:7} {value.selected_subcarriers:2}")
    for (node_id, receiver), loss in state.pipeline.receiver_losses().items():
        label = "Newo" if node_id == 1 else ("Newo2" if node_id == 2 else f"node{node_id}")
        lines.append(f"RX {label} {receiver}: gaps={loss['gaps']} duplicates={loss['duplicates']}")
    lines += ["", "R record  S stop  E event  P placement  O occupancy  A activity  Q quit",
              "Labels are operator annotations. RF_CHANGE/MOTION_CANDIDATE are not person detection or localization."]
    return "\x1b[2J\x1b[H" + "\n".join(lines)


def replay_items(path: Path, speed: float) -> Iterator[ArchivedRecord]:
    previous: int | None = None
    for item in iter_archive(path / "frames.ncsi" if path.is_dir() else path):
        if previous is not None and speed > 0:
            delay = (item.host_monotonic_ns - previous) / 1e9 / speed
            if delay > 0:
                time.sleep(delay)
        previous = item.host_monotonic_ns
        yield item


def run_live(args: object, renderer: Callable[..., str] = render,
             field_mode: bool = False) -> int:
    state = LiveState(args.top_k, args.window_seconds, args.placement, args.settle_seconds)
    calibration_path = Path(args.calibration_file)
    if calibration_path.is_file() and not args.calibrate:
        state.pipeline.load_calibration(json.loads(calibration_path.read_text(encoding="utf-8")),
                                        state.placement, args.room_id)
    calibration_deadline = None
    selection_deadline = None
    if args.calibrate:
        state.pipeline.begin_calibration()
        calibration_deadline = time.monotonic() + args.calibrate
        selection_deadline = time.monotonic() + args.calibrate * 0.4

    sock: socket.socket | None = None
    source: Iterator[ArchivedRecord] | None = None
    if args.replay:
        source = replay_items(Path(args.replay), args.speed)
    else:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.receive_buffer)
        sock.bind((args.bind, args.port))
        sock.settimeout(0.1)

    recorder: SessionRecorder | None = None
    last_render = 0.0
    terminal = TerminalInput()
    terminal.__enter__()
    try:
        running = True
        while running:
            item = None
            if source is not None:
                try:
                    item = next(source)
                except StopIteration:
                    break
                raw, mono, wall, sender = item.record, item.host_monotonic_ns, item.host_wall_ns, (item.source_ip, item.source_port)
            else:
                try:
                    assert sock is not None
                    raw, sender = sock.recvfrom(4097)
                    mono, wall = time.monotonic_ns(), time.time_ns()
                except socket.timeout:
                    raw = b""
            if raw:
                try:
                    record = decode(raw)
                except ProtocolError:
                    state.rejected += 1
                else:
                    state.add(record, mono)
                    if recorder is not None:
                        recorder.append(raw, mono, wall, sender, record)

            now = time.monotonic()
            if selection_deadline is not None and now >= selection_deadline:
                state.pipeline.freeze_calibration_selection()
                selection_deadline = None
            if calibration_deadline is not None and now >= calibration_deadline:
                document = state.pipeline.calibration_document(state.placement, args.room_id)
                document["created_at"] = utc_now()
                calibration_path.parent.mkdir(parents=True, exist_ok=True)
                atomic_json(calibration_path, document)
                state.pipeline.load_calibration(document, state.placement, args.room_id)
                calibration_deadline = None
            if now - last_render >= 0.25:
                sys.stdout.write(renderer(state, recorder, None if calibration_deadline is None else calibration_deadline - now))
                sys.stdout.flush()
                last_render = now

            key = terminal.read_key()
            if not key:
                continue
            key = key.upper()
            if key == "Q":
                running = False
            elif key == "R" and recorder is None:
                recorder = SessionRecorder(Path(args.dataset_dir), args.room_id, args.scenario,
                                           state.placement, state.occupancy, state.activity)
            elif key == "S" and recorder is not None:
                recorder.close()
                recorder = None
            elif key == "E" and recorder is not None:
                event = prompt("Event " + "/".join(EVENT_TYPES), "CUSTOM", terminal).upper()
                if event not in EVENT_TYPES:
                    event = "CUSTOM"
                note = prompt("Event note", "", terminal) if event == "CUSTOM" else ""
                recorder.event("marker", marker=event, note=note, placement=state.placement,
                               occupancy=state.occupancy, activity=state.activity)
            elif key == "P":
                if recorder is not None:
                    recorder.event("repositioning_started", old_placement=state.placement)
                    recorder.close("placement_change_excludes_antenna_motion")
                    recorder = None
                state.change_placement(prompt("Newo2 placement", state.placement, terminal))
            elif key == "O":
                state.occupancy = prompt("Occupancy label", state.occupancy, terminal)
                if recorder:
                    recorder.event("occupancy_changed", value=state.occupancy)
            elif key == "A":
                state.activity = prompt("Activity label", state.activity, terminal)
                if recorder:
                    recorder.event("activity_changed", value=state.activity)
    except KeyboardInterrupt:
        pass
    finally:
        terminal.restore()
        if recorder is not None:
            recorder.close("console_exit")
        if sock is not None:
            sock.close()
        sys.stdout.write("\x1b[0m\n")
    return 0
