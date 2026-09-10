from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import socket
import sys
import time
import uuid

from .archive import ArchiveWriter, iter_archive
from .protocol import CsiRecord, PATH_NAMES, ProtocolError, decode, mac_text
from .metadata import capture_metadata
from .statistics import CaptureStats

SCENARIOS = (
    "EMPTY", "DIAGNOSTIC", "BACKGROUND", "ENTER", "EXIT", "WALK_DOOR_CENTER", "WALK_CENTER_DOOR",
    "WALK_LEFT_RIGHT", "WALK_RIGHT_LEFT", "STAND", "SIT", "LIE", "WAVE",
    "TURN", "FAST_WALK", "SLOW_WALK",
)
DEFAULT_DATASET_DIR = Path(__file__).resolve().parents[1] / "datasets"


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def archive_path(value: str) -> Path:
    path = Path(value)
    return path / "frames.ncsi" if path.is_dir() else path


def write_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def session_identifier(explicit: str | None, scenario: str) -> str:
    if explicit:
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", explicit):
            raise SystemExit("--session-id must be 1-128 safe filename characters")
        return explicit
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return f"{stamp}-{scenario.lower()}-{uuid.uuid4().hex[:8]}"


def parse_mac(value: str) -> bytes:
    if not re.fullmatch(r"[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}", value):
        raise SystemExit(f"invalid MAC address: {value}")
    return bytes.fromhex(value.replace(":", ""))


def path_mapping(args: argparse.Namespace) -> dict[int, tuple[bytes, bytes]] | None:
    values = (args.router_bssid, args.newo_mac, args.newo2_mac)
    if not any(values):
        return None
    if not all(values):
        raise SystemExit("--router-bssid, --newo-mac, and --newo2-mac must be supplied together")
    router, newo, newo2 = map(parse_mac, values)
    return {1: (newo, router), 2: (newo2, router), 3: (newo, newo2)}


def mapping_metadata(mapping: dict[int, tuple[bytes, bytes]] | None) -> dict[str, object] | None:
    if mapping is None:
        return None
    return {PATH_NAMES[path]: {"receiver_mac": mac_text(pair[0]),
                               "source_mac": mac_text(pair[1])}
            for path, pair in mapping.items()}


def collection_metadata(args: argparse.Namespace, session_id: str,
                        mapping: dict[int, tuple[bytes, bytes]] | None,
                        started_at: str, started_monotonic_ns: int,
                        receive_buffer_bytes: int) -> dict[str, object]:
    """Build canonical capture metadata without deriving operator labels."""
    return capture_metadata(
        session_id=session_id, room_id=args.room_id, scenario=args.scenario,
        placement=args.placement, occupancy=args.person, activity=args.activity,
        zone=args.zone, camera_frame_id=args.camera_frame_id, notes=args.notes,
        path_mapping=mapping_metadata(mapping), started_at=started_at,
        started_monotonic_ns=started_monotonic_ns,
        receive_buffer_bytes=receive_buffer_bytes)


def collect(args: argparse.Namespace) -> int:
    mapping = path_mapping(args)
    if args.placement is None:
        print("WARNING: placement omitted; calibrated evaluation may not be possible",
              file=sys.stderr)
    session_id = session_identifier(args.session_id, args.scenario)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.receive_buffer)
    sock.bind((args.bind, args.port))
    sock.settimeout(0.5)
    session_dir = Path(args.dataset_dir) / session_id
    try:
        session_dir.mkdir(parents=True, exist_ok=False)
    except Exception:
        sock.close()
        raise
    started_at = utc_now()
    started_monotonic_ns = time.monotonic_ns()
    metadata = collection_metadata(
        args, session_id, mapping, started_at, started_monotonic_ns,
        sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF))
    write_json(session_dir / "session.json", metadata)
    events = (session_dir / "events.jsonl").open("x", encoding="utf-8")
    events.write(json.dumps({"event": "capture_started", "at": started_at,
                             "bind": args.bind, "port": args.port}) + "\n")
    stats = CaptureStats()
    deadline = None if args.duration is None else time.monotonic() + args.duration
    print(f"capturing session {session_id} on {args.bind}:{args.port}")
    try:
        with ArchiveWriter(session_dir / "frames.ncsi") as archive:
            while deadline is None or time.monotonic() < deadline:
                try:
                    payload, source = sock.recvfrom(4097)
                except socket.timeout:
                    continue
                received_monotonic_ns = time.monotonic_ns()
                received_wall_ns = time.time_ns()
                try:
                    record = decode(payload)
                except ProtocolError:
                    stats.reject("protocol")
                    continue
                if isinstance(record, CsiRecord) and mapping is not None:
                    expected = mapping.get(record.path_id)
                    if expected is None or expected != (record.receiver_mac, record.source_mac):
                        stats.reject("path_identity_mismatch")
                        continue
                archive.append(payload, received_monotonic_ns, received_wall_ns, source)
                stats.add(record, received_monotonic_ns)
                if stats.total_records % 100 == 0:
                    archive.flush()
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        ended_at = utc_now()
        ended_monotonic_ns = time.monotonic_ns()
        metadata["capture_ended_at"] = ended_at
        metadata["capture_ended_monotonic_ns"] = ended_monotonic_ns
        write_json(session_dir / "session.json", metadata)
        summary = stats.as_dict()
        summary.update({"schema_version": 1, "session_id": session_id,
                        "capture_started_at": started_at, "capture_ended_at": ended_at})
        write_json(session_dir / "summary.json", summary)
        events.write(json.dumps({"event": "capture_ended", "at": ended_at,
                                 "record_counts": summary["record_counts"]}) + "\n")
        events.close()
    print(json.dumps(stats.as_dict(), indent=2, sort_keys=True))
    return 0


def load_stats(path: Path) -> CaptureStats:
    stats = CaptureStats()
    for item in iter_archive(path):
        stats.add(decode(item.record), item.host_monotonic_ns)
    return stats


def inspect_command(args: argparse.Namespace) -> int:
    session = Path(args.session)
    result = load_stats(archive_path(args.session)).as_dict()
    summary_path = session / "summary.json"
    if summary_path.is_file():
        capture_summary = json.loads(summary_path.read_text(encoding="utf-8"))
        result["capture_rejected_datagrams"] = capture_summary.get(
            "record_counts", {}).get("rejected_datagrams", 0)
        result["capture_rejection_reasons"] = capture_summary.get("rejection_reasons", {})
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    counts = result["record_counts"]
    print(f"records={counts['total']} csi={counts['csi']} status={counts['status']} "
          f"sync={counts['sync']} diagnostic={counts['diagnostic']} "
          f"duration={result['duration_seconds']:.3f}s")
    gaps = sum(result["sequence_gap_estimate_by_receiver"].values())
    print(f"sequence_gap_estimate={gaps} "
          f"capture_rejected={result.get('capture_rejected_datagrams', 'unavailable')}")
    for name, path in result["paths"].items():
        rssi = path["rssi_dbm"]
        print(f"{name}: frames={path['frame_count']} rate={path['packet_rate_hz']:.3f}Hz "
              f"rssi={rssi['min']}/{rssi['mean']}/{rssi['max']}dBm "
              f"lengths={path['csi_length_distribution']} channels={path['channels']} "
              f"sources={','.join(path['source_macs'])}")
    return 0


def replay(args: argparse.Namespace) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    previous_ns: int | None = None
    sent = 0
    try:
        for item in iter_archive(archive_path(args.session)):
            record = decode(item.record)
            if args.csi_only and not isinstance(record, CsiRecord):
                continue
            if previous_ns is not None and args.speed > 0:
                delay = (item.host_monotonic_ns - previous_ns) / 1e9 / args.speed
                if delay > 0:
                    time.sleep(delay)
            sock.sendto(item.record, (args.host, args.port))
            previous_ns = item.host_monotonic_ns
            sent += 1
    finally:
        sock.close()
    print(f"replayed {sent} records to {args.host}:{args.port}")
    return 0


def selected_indices(text: str, count: int) -> list[int]:
    if text.lower() == "all":
        return list(range(count))
    values = sorted({int(value) for value in text.split(",")})
    if any(value < 0 or value >= count for value in values):
        raise ValueError(f"subcarrier selection outside 0..{count - 1}")
    return values


def export(args: argparse.Namespace) -> int:
    rows = 0
    with Path(args.output).open("x", newline="", encoding="utf-8") as target:
        writer = csv.writer(target)
        writer.writerow(("host_monotonic_ns", "host_wall_ns", "path", "node_id", "sequence",
                         "receiver_timestamp_us", "subcarrier", "imag", "real",
                         "amplitude", "phase_radians"))
        for item in iter_archive(archive_path(args.session)):
            record = decode(item.record)
            if not isinstance(record, CsiRecord):
                continue
            for index in selected_indices(args.subcarriers, record.subcarrier_item_count):
                imag = int.from_bytes(record.iq_bytes[index * 2:index * 2 + 1], "little", signed=True)
                real = int.from_bytes(record.iq_bytes[index * 2 + 1:index * 2 + 2], "little", signed=True)
                writer.writerow((item.host_monotonic_ns, item.host_wall_ns,
                                 PATH_NAMES.get(record.path_id, f"PATH_{record.path_id}"),
                                 record.node_id, record.sequence, record.timestamp_us,
                                 index, imag, real, math.hypot(real, imag),
                                 math.atan2(imag, real)))
                rows += 1
    print(f"exported {rows} derived subcarrier rows to {args.output}")
    return 0


def live(args: argparse.Namespace) -> int:
    from .live import run_live
    return run_live(args)


def field(args: argparse.Namespace) -> int:
    from .field import run_field
    return run_field(args)


def _evaluation_calibration(path: str | None) -> dict[str, object] | None:
    if not path:
        return None
    calibration_path = Path(path)
    if not calibration_path.is_file():
        raise SystemExit(f"calibration not found: {calibration_path}")
    return json.loads(calibration_path.read_text(encoding="utf-8"))


def evaluate_command(args: argparse.Namespace) -> int:
    from .evaluate import evaluate_archive, human_report
    calibration = _evaluation_calibration(args.calibration_file)
    results = [evaluate_archive(value, args.window_seconds, calibration,
                                Path(args.annotations_dir), getattr(args, "top_k", 24))
               for value in args.datasets]
    if args.json:
        print(json.dumps({"schema_version": 1, "datasets": results}, indent=2, sort_keys=True))
    else:
        print("\n\n".join(human_report(result) for result in results))
    return 0


def compare_command(args: argparse.Namespace) -> int:
    from .evaluate import evaluate_archive, human_comparison
    calibration = _evaluation_calibration(args.calibration_file)
    results = [evaluate_archive(value, args.window_seconds, calibration,
                                Path(args.annotations_dir), getattr(args, "top_k", 24))
               for value in args.datasets]
    comparison = {"schema_version": 1, "window_seconds": args.window_seconds,
                  "datasets": results,
                  "comparison_boundary": "descriptive side-by-side evidence; labels are not inferred truth"}
    if args.json:
        print(json.dumps(comparison, indent=2, sort_keys=True))
    else:
        print(human_comparison(results))
    return 0


def annotate_command(args: argparse.Namespace) -> int:
    from .annotations import append_annotation
    try:
        record = append_annotation(Path(args.session), args.label, args.note,
                                   args.start, args.end, Path(args.annotations_dir))
    except ValueError as error:
        raise SystemExit(str(error)) from error
    print(json.dumps(record, indent=2, sort_keys=True))
    return 0


def catalog_command(args: argparse.Namespace) -> int:
    from .catalog import build_catalog, human_catalog
    calibration = _evaluation_calibration(args.calibration_file)
    try:
        document = build_catalog(Path(args.root), calibration, Path(args.annotations_dir),
                                 args.top_k, args.window_seconds)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    print(json.dumps(document, indent=2, sort_keys=True) if args.json else human_catalog(document))
    return 0


def add_monitor_arguments(command: argparse.ArgumentParser) -> None:
    command.add_argument("--bind", default="0.0.0.0")
    command.add_argument("--port", type=int, default=5005)
    command.add_argument("--receive-buffer", type=int, default=4 * 1024 * 1024)
    command.add_argument("--dataset-dir", default=str(DEFAULT_DATASET_DIR))
    command.add_argument("--room-id", default="UNSPECIFIED")
    command.add_argument("--scenario", choices=SCENARIOS, default="DIAGNOSTIC")
    command.add_argument("--placement", default="UNSPECIFIED")
    command.add_argument("--top-k", type=int, default=24)
    command.add_argument("--window-seconds", type=float, default=2.0)
    command.add_argument("--settle-seconds", type=float, default=10.0)
    command.add_argument("--calibrate", type=float, metavar="SECONDS")
    command.add_argument("--calibration-file", default=str(DEFAULT_DATASET_DIR.parent / "calibrations" / "baseline.json"))
    command.add_argument("--replay", help="session/archive input instead of UDP")
    command.add_argument("--speed", type=float, default=1.0,
                         help="archive replay speed; 0 disables delays")


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(prog="python -m newo_csi")
    commands = root.add_subparsers(dest="command", required=True)
    capture = commands.add_parser("collect", help="capture a labeled UDP session")
    capture.add_argument("--bind", default="0.0.0.0")
    capture.add_argument("--port", type=int, default=5005)
    capture.add_argument("--receive-buffer", type=int, default=4 * 1024 * 1024)
    capture.add_argument("--dataset-dir", default=str(DEFAULT_DATASET_DIR))
    capture.add_argument("--session-id")
    capture.add_argument("--room-id", required=True)
    capture.add_argument("--scenario", required=True, choices=SCENARIOS)
    capture.add_argument("--person")
    capture.add_argument("--zone")
    capture.add_argument("--activity")
    capture.add_argument("--placement",
                         help="canonical physical placement identity; never inferred")
    capture.add_argument("--camera-frame-id")
    capture.add_argument("--notes")
    capture.add_argument("--router-bssid")
    capture.add_argument("--newo-mac")
    capture.add_argument("--newo2-mac")
    capture.add_argument("--duration", type=float)
    capture.set_defaults(handler=collect)

    show = commands.add_parser("inspect", help="print concise archive statistics")
    show.add_argument("session")
    show.add_argument("--json", action="store_true")
    show.set_defaults(handler=inspect_command)

    play = commands.add_parser("replay", help="replay original records over UDP")
    play.add_argument("session")
    play.add_argument("--host", default="127.0.0.1")
    play.add_argument("--port", type=int, default=5005)
    play.add_argument("--speed", type=float, default=1.0,
                      help="1=recorded timing, 2=twice speed, 0=no delays")
    play.add_argument("--csi-only", action="store_true")
    play.set_defaults(handler=replay)

    derive = commands.add_parser("export", help="export derived I/Q amplitude/phase CSV")
    derive.add_argument("session")
    derive.add_argument("--output", required=True)
    derive.add_argument("--subcarriers", required=True,
                        help="comma-separated zero-based indices, or 'all'")
    derive.set_defaults(handler=export)

    console = commands.add_parser("live", help="live/replay CSI DSP research console")
    add_monitor_arguments(console)
    console.set_defaults(handler=live)

    phone = commands.add_parser("field", help="compact Termux-first field monitor")
    add_monitor_arguments(phone)
    phone.set_defaults(handler=field)

    evaluate = commands.add_parser("evaluate", help="offline shared-DSP dataset evaluation")
    evaluate.add_argument("datasets", nargs="+")
    evaluate.add_argument("--window-seconds", type=float, default=1.0)
    evaluate.add_argument("--top-k", type=int, default=24)
    evaluate.add_argument("--calibration-file")
    evaluate.add_argument("--annotations-dir", default=str(DEFAULT_DATASET_DIR.parent / "operator-annotations"))
    evaluate.add_argument("--json", action="store_true")
    evaluate.set_defaults(handler=evaluate_command)

    compare = commands.add_parser("compare", help="side-by-side offline dataset evaluation")
    compare.add_argument("datasets", nargs="+")
    compare.add_argument("--window-seconds", type=float, default=1.0)
    compare.add_argument("--top-k", type=int, default=24)
    compare.add_argument("--calibration-file")
    compare.add_argument("--annotations-dir", default=str(DEFAULT_DATASET_DIR.parent / "operator-annotations"))
    compare.add_argument("--json", action="store_true")
    compare.set_defaults(handler=compare_command)

    annotate = commands.add_parser("annotate", help="append non-destructive operator ground truth")
    annotate.add_argument("session")
    annotate.add_argument("--label", required=True)
    annotate.add_argument("--note")
    annotate.add_argument("--start", type=float)
    annotate.add_argument("--end", type=float)
    annotate.add_argument("--annotations-dir", default=str(DEFAULT_DATASET_DIR.parent / "operator-annotations"))
    annotate.set_defaults(handler=annotate_command)

    catalog = commands.add_parser("catalog", help="read-only dataset inventory")
    catalog.add_argument("root", nargs="?", default=str(DEFAULT_DATASET_DIR))
    catalog.add_argument("--calibration-file")
    catalog.add_argument("--top-k", type=int, default=24)
    catalog.add_argument("--window-seconds", type=float, default=2.0)
    catalog.add_argument("--annotations-dir", default=str(DEFAULT_DATASET_DIR.parent / "operator-annotations"))
    catalog.add_argument("--json", action="store_true")
    catalog.set_defaults(handler=catalog_command)
    return root


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if getattr(args, "duration", None) is not None and args.duration <= 0:
        raise SystemExit("--duration must be positive")
    if getattr(args, "receive_buffer", 1) <= 0:
        raise SystemExit("--receive-buffer must be positive")
    if getattr(args, "speed", 0) < 0:
        raise SystemExit("--speed must be non-negative")
    if getattr(args, "top_k", 1) <= 0 or getattr(args, "window_seconds", 1) <= 0:
        raise SystemExit("--top-k and --window-seconds must be positive")
    if getattr(args, "settle_seconds", 0) < 0:
        raise SystemExit("--settle-seconds must be non-negative")
    if getattr(args, "calibrate", None) is not None and args.calibrate <= 0:
        raise SystemExit("--calibrate must be positive")
    return args.handler(args)
