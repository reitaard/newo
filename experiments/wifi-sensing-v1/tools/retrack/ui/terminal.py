from __future__ import annotations

import shutil
import sys
import time

from newo_csi.live import TerminalInput, prompt


def _clip(value: str, width: int) -> str:
    return value if len(value) <= width else value[:max(0, width - 1)] + "…"


def render(snapshot: dict[str, object], *, width: int | None = None) -> str:
    width = max(24, width or shutil.get_terminal_size((40, 24)).columns)
    nodes = snapshot.get("nodes", [])
    slots = list(nodes[:4]) + [None] * max(0, 4 - len(nodes))
    sync = snapshot.get("sync", {})
    geometry = snapshot.get("geometry", {})
    paths = snapshot.get("paths", {})
    state = sync.get("state", "SYNC_UNSYNCED").replace("SYNC_", "")
    lines = [f"RETRACK  {snapshot.get('room', 'UNSPECIFIED')}",
             f"TRACK {snapshot.get('track')}  REC {'ON' if snapshot.get('recording') else 'OFF'}  SYNC {state}",
             f"Session {snapshot.get('session_id') or '--'}  Frames {snapshot.get('records', 0)}"]
    for index, node in enumerate(slots, 1):
        if node is None:
            lines.append(f"N{index} Empty")
        else:
            lines.append(f"N{index} {node.get('friendly_name', node.get('node_id'))}  {node.get('node_id')}  {node.get('last_ip') or '--'}")
    lines.append("")
    for name, path in paths.items():
        score = path.get("score")
        lines.append(f"{name[:12]:12} {path.get('hz', 0):5.1f}Hz {path.get('rssi_dbm', '--')}dBm "
                     f"{path.get('quality', '--')} {'--' if score is None else f'{score:.2f}'}/{path.get('state', '--')}")
    if not paths:
        lines.append("RF paths waiting for TRACK ON / local data")
    drift = sync.get("drift_ppm")
    lines.extend(["", f"Geometry {geometry.get('placement')}  {geometry.get('state')}",
                  f"Sync offset {sync.get('final_offset_us', '--')}us  drift {'--' if drift is None else f'{drift:.1f}ppm'}",
                  f"Publisher {snapshot.get('publisher')}  WAN not required",
                  "T track  R record  E event  P placement  C calibration  Q quit"])
    return "\x1b[2J\x1b[H" + "\n".join(_clip(line, width) for line in lines)


def run_tui(runtime) -> int:
    terminal = TerminalInput()
    terminal.__enter__()
    last_render = 0.0
    try:
        try:
            runtime.discover_leader()
        except TimeoutError:
            pass
        running = True
        while running:
            runtime.poll()
            now = time.monotonic()
            if now - last_render >= 0.25:
                sys.stdout.write(render(runtime.core.snapshot()))
                sys.stdout.flush()
                last_render = now
            key = terminal.read_key()
            if not key:
                continue
            key = key.upper()
            if key == "Q":
                running = False
            elif key == "T":
                runtime.set_track(runtime.core.track_actual != "ON")
            elif key == "R":
                if runtime.core.recording:
                    runtime.core.stop_recording()
                else:
                    runtime.core.start_recording()
            elif key == "E" and runtime.core.recording:
                runtime.core.add_event(prompt("Ground-truth event", "CUSTOM", terminal),
                                       prompt("Note", "", terminal))
            elif key == "P":
                runtime.core.change_placement(prompt("Placement", runtime.core.geometry.placement, terminal))
            elif key == "C":
                # Phase 7A carries the geometry state but deliberately does not start Phase 7B model work.
                pass
    except KeyboardInterrupt:
        pass
    finally:
        terminal.restore()
        if runtime.core.recording:
            runtime.core.stop_recording("INCOMPLETE/RECOVERED")
        runtime.close()
        sys.stdout.write("\x1b[0m\n")
    return 0
