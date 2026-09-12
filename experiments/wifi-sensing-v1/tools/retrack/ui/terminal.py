from __future__ import annotations

import shutil
import sys
import time

from newo_csi.live import TerminalInput, prompt


def _clip(value: str, width: int) -> str:
    return value if len(value) <= width else value[:max(0, width - 1)] + "…"


def render(snapshot: dict[str, object], *, width: int | None = None,
           client_id: str | None = None) -> str:
    width = max(24, width or shutil.get_terminal_size((40, 24)).columns)
    nodes = snapshot.get("nodes", [])
    slots = list(nodes[:4]) + [None] * max(0, 4 - len(nodes))
    sync = snapshot.get("sync", {})
    geometry = snapshot.get("geometry", {})
    paths = snapshot.get("paths", {})
    calibration = snapshot.get("calibration", {})
    calibration_progress = snapshot.get("calibration_progress", {})
    client_api = snapshot.get("client_api", {})
    state = sync.get("state", "SYNC_UNSYNCED").replace("SYNC_", "")
    lines = [f"RETRACK  {snapshot.get('room', 'UNSPECIFIED')}",
             f"TRACK {snapshot.get('track')}  REC {'ON' if snapshot.get('recording') else 'OFF'}  SYNC {state}",
             f"Session {snapshot.get('session_id') or '--'}  {snapshot.get('recording_elapsed_seconds', 0):.0f}s",
             f"CONTROL {'THIS CLIENT' if client_id and client_api.get('controller_id') == client_id else 'VIEWER'}  "
             f"Frames {snapshot.get('records', 0)}"]
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
    remaining = calibration_progress.get("remaining_seconds")
    cal_line = (f"CAL {calibration_progress.get('stage')}  {remaining:.0f}s"
                if calibration_progress.get("active") and isinstance(remaining, (int, float))
                else f"CAL {calibration.get('status', 'MISSING')}  {calibration.get('reason') or ''}")
    unmatched = calibration.get("mismatched_geometries", [])
    lines.extend(["", f"Geometry {geometry.get('placement')}  {geometry.get('state')}",
                  cal_line,
                  f"CAL exact {len(calibration.get('matched_paths', []))} unmatched {len(unmatched)}",
                  f"Sync offset {sync.get('final_offset_us', '--')}us  drift {'--' if drift is None else f'{drift:.1f}ppm'}",
                  f"Publisher {snapshot.get('publisher')}  WAN not required",
                  "T track  R record  E event  P placement  C calibration  Q detach"])
    return "\x1b[2J\x1b[H" + "\n".join(_clip(line, width) for line in lines)


def run_tui(client) -> int:
    terminal = TerminalInput()
    terminal.__enter__()
    last_render = 0.0
    last_error = ""
    try:
        running = True
        while running:
            client.maintain()
            now = time.monotonic()
            if now - last_render >= 0.25:
                body = render(client.snapshot(), client_id=client.client_id)
                if last_error:
                    body += "\n" + _clip(f"ERROR {last_error}", shutil.get_terminal_size((40, 24)).columns)
                sys.stdout.write(body)
                sys.stdout.flush()
                last_render = now
            key = terminal.read_key()
            if not key:
                continue
            key = key.upper()
            if key == "Q":
                running = False
            elif key == "T":
                snapshot = client.snapshot()
                result = client.mutate("TRACK_SET", state="OFF" if snapshot.get("track") == "ON" else "ON")
                last_error = "" if result.get("ok") else str(result.get("error"))
            elif key == "R":
                snapshot = client.snapshot()
                result = client.mutate("RECORD_SET", state="OFF" if snapshot.get("recording") else "ON")
                last_error = "" if result.get("ok") else str(result.get("error"))
            elif key == "E" and client.snapshot().get("recording"):
                result = client.mutate("EVENT", label=prompt("Ground-truth event", "CUSTOM", terminal),
                                       note=prompt("Note", "", terminal))
                last_error = "" if result.get("ok") else str(result.get("error"))
            elif key == "P":
                current = client.snapshot().get("geometry", {}).get("placement", "UNSPECIFIED")
                result = client.mutate("PLACEMENT_SET", placement=prompt("Placement", current, terminal))
                last_error = "" if result.get("ok") else str(result.get("error"))
            elif key == "C":
                active = client.snapshot().get("calibration_progress", {}).get("active")
                result = client.mutate("CALIBRATION_CANCEL" if active else "CALIBRATION_START")
                last_error = "" if result.get("ok") else str(result.get("error"))
    except KeyboardInterrupt:
        pass
    finally:
        terminal.restore()
        client.close()
        sys.stdout.write("\x1b[0m\n")
    return 0
