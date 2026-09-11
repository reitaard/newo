from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from retrack.collector import NetworkRuntime, ReTrackCore
from retrack.config import load_config
from retrack.control import LocalTrackClient
from retrack.nodes import NodeRegistry
from retrack.replay import replay_session
from retrack.sessions import catalog_sessions
from retrack.storage import recover_session
from retrack.ui import run_tui


def _core(args):
    config = load_config(Path(args.config).expanduser() if args.config else None, room=args.room)
    if getattr(args, "data_dir", None):
        config.data_dir = Path(args.data_dir).expanduser()
    if getattr(args, "leader", None):
        config.leader_host = None if args.leader == "auto" else args.leader
    registry = NodeRegistry(config.registry_file)
    core = ReTrackCore(data_dir=config.data_dir, registry=registry, room=config.room,
                       placement=config.placement, top_k=config.top_k,
                       window_seconds=config.window_seconds, settle_seconds=config.settle_seconds,
                       rotate_bytes=config.rotate_bytes, publisher=None)
    return config, core


def run_command(args) -> int:
    config, core = _core(args)
    control = LocalTrackClient(config.leader_host, port=config.control_port)
    runtime = NetworkRuntime(core, control, bind=config.bind, data_port=config.data_port)
    return run_tui(runtime)


def daemon_command(args) -> int:
    config, core = _core(args)
    control = LocalTrackClient(config.leader_host, port=config.control_port)
    runtime = NetworkRuntime(core, control, bind=config.bind, data_port=config.data_port)
    try:
        runtime.discover_leader()
        if args.track_on:
            ack = runtime.set_track(True)
            if not ack.accepted or ack.actual != "ON":
                raise RuntimeError(f"leader rejected TRACK ON: {ack.error}")
        if args.record:
            core.start_recording(args.label)
        while True:
            runtime.poll()
    except KeyboardInterrupt:
        return 0
    finally:
        if core.recording:
            core.stop_recording()
        if args.track_on and core.track_actual == "ON":
            try:
                runtime.set_track(False)
            except TimeoutError:
                pass
        runtime.close()


def replay_command(args) -> int:
    _config, core = _core(args)
    snapshot = replay_session(core, Path(args.session), args.speed)
    print(json.dumps(snapshot, indent=2, sort_keys=True))
    return 0


def catalog_command(args) -> int:
    config, _core_value = _core(args)
    rows = catalog_sessions(config.data_dir / "sessions", args.room)
    if args.json:
        print(json.dumps({"schema": "retrack_catalog_v1", "sessions": rows}, indent=2, sort_keys=True))
    else:
        current = None
        for row in rows:
            if row["room"] != current:
                current = row["room"]
                print(current)
            print(f"  {row['started_at']}  {row['session_id']}  {row['state']}  {row['records']} records")
    return 0


def recover_command(args) -> int:
    print(json.dumps(recover_session(Path(args.session)), indent=2, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(prog="retrack", description="Local-first Newo RF runtime")
    root.add_argument("--config", help="retrack_config_v1 JSON")
    root.add_argument("--room", default=None, help="named room profile")
    root.add_argument("--data-dir")
    commands = root.add_subparsers(dest="command")
    live = commands.add_parser("run", help="start the local runtime and terminal UI")
    live.add_argument("--leader", default=None, help="leader IPv4 address or auto")
    live.set_defaults(handler=run_command)
    daemon = commands.add_parser("daemon", help="headless local runtime (initial no-IPC boundary)")
    daemon.add_argument("--leader", default=None, help="leader IPv4 address or auto")
    daemon.add_argument("--track-on", action="store_true")
    daemon.add_argument("--record", action="store_true")
    daemon.add_argument("--label")
    daemon.set_defaults(handler=daemon_command)
    replay = commands.add_parser("replay", help="replay a ReTrack session through the shared DSP")
    replay.add_argument("session")
    replay.add_argument("--speed", type=float, default=0.0)
    replay.set_defaults(handler=replay_command)
    catalog = commands.add_parser("catalog", help="list durable local sessions")
    catalog.add_argument("--json", action="store_true")
    catalog.set_defaults(handler=catalog_command)
    recover = commands.add_parser("recover", help="mark an interrupted session recoverable")
    recover.add_argument("session")
    recover.set_defaults(handler=recover_command)
    root.set_defaults(handler=run_command, leader=None)
    return root


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    return args.handler(args)


def daemon_main(argv: list[str] | None = None) -> int:
    values = list(sys.argv[1:] if argv is None else argv)
    globals_: list[str] = []
    remainder: list[str] = []
    index = 0
    while index < len(values):
        if values[index] in ("--config", "--room", "--data-dir") and index + 1 < len(values):
            globals_.extend(values[index:index + 2])
            index += 2
        else:
            remainder.append(values[index])
            index += 1
    args = parser().parse_args([*globals_, "daemon", *remainder])
    return args.handler(args)
