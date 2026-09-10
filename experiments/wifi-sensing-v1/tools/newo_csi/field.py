"""Compact Termux-first presentation using the shared live engine."""

from __future__ import annotations

import shutil
import time

from .live import LiveState, SessionRecorder, run_live
from .protocol import PATH_NAMES


def _clip(value: str, width: int) -> str:
    if width <= 0:
        return ""
    return value if len(value) <= width else value[:max(0, width - 1)] + "…"


def render_field(state: LiveState, recorder: SessionRecorder | None,
                 calibration_left: float | None, width: int | None = None) -> str:
    width = width or shutil.get_terminal_size((40, 24)).columns
    width = max(20, width)
    now = time.monotonic()
    online = lambda node: "ON" if now - state.node_seen.get(node, -1e9) < 5 else "OFF"
    room, confidence = state.pipeline.fused(state.repositioning)
    calibration = state.pipeline.calibration_report()
    if recorder:
        elapsed = max(0.0, now - recorder.started_monotonic)
        recording = f"REC {elapsed:5.1f}s"
    else:
        recording = "REC OFF"
    lines = ["NEwo CSI FIELD",
             f"N:{online(1)} N2:{online(2)} UDP:{'OK' if now-state.last_udp_monotonic < 3 else '--'} {recording}",
             f"COL:{state.collector_state}",
             f"P:{state.placement}",
             f"O:{state.occupancy} A:{state.activity}",
             f"ROOM:{room} {confidence:.2f}  PRES:UNSUPPORTED",
             f"CAL:{calibration['status']} {calibration['reason'] or ''}"]
    if calibration_left is not None:
        lines.append(f"CAL {max(0.0, calibration_left):.0f}s")
    snapshots = state.pipeline.snapshots()
    if width < 50:
        lines.append("PATH       Hz RSSI Q  SCORE/STATE")
        short = {1: "R-N", 2: "R-N2", 3: "N2-N"}
        for path in (1, 2, 3):
            item = snapshots.get(path)
            if item is None:
                lines.append(f"{short[path]:5}      --  -- -- LOW_CONF")
            else:
                score = "--" if item.motion_score is None else f"{item.motion_score:.1f}"
                lines.append(f"{short[path]:5} {item.sample_rate_hz:5.1f} {str(item.rssi_dbm):>4} "
                             f"{item.signal_quality[0]:1} {score:>4}/{item.motion_state[:10]}")
        lines.append(f"PKT:{state.records} REJ:{state.rejected}")
    else:
        lines.append("PATH            Hz  RSSI QUAL SCORE/STATE")
        for path in (1, 2, 3):
            item = snapshots.get(path)
            if item is None:
                lines.append(f"{PATH_NAMES[path]:15} --    --  --   LOW_CONFIDENCE")
            else:
                score = "--" if item.motion_score is None else f"{item.motion_score:.1f}"
                lines.append(f"{item.path_name:15} {item.sample_rate_hz:4.1f} {str(item.rssi_dbm):>4} "
                             f"{item.signal_quality:4} {score:>4}/{item.motion_state:14}")
        lines.append(f"packets={state.records} rejected={state.rejected}")
    for (node_id, _receiver), loss in state.pipeline.receiver_losses().items():
        label = "Newo" if node_id == 1 else ("Newo2" if node_id == 2 else f"N{node_id}")
        lines.append(f"RX {label}: gaps={loss['gaps']} dup={loss['duplicates']}")
    if width < 50:
        lines += ["R rec  S stop", "E mark P place", "O occ A act Q quit"]
    else:
        lines.append("R rec S stop E mark P place O occ A act Q quit")
    return "\x1b[2J\x1b[H" + "\n".join(_clip(line, width) for line in lines)


def run_field(args: object) -> int:
    return run_live(args, renderer=render_field, field_mode=True)
