"""Second-stage analysis for motion_decode.py CSV output.

Consumes the small 10-second motion timeline, not frames.ncsi. It reports
state durations, sustained low-motion stretches, merged high-motion bursts,
per-path event signatures, and path-to-path correlation. This is relative RF
motion analysis only; it does not infer biological sleep stages or identity.
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timedelta
import json
import math
from pathlib import Path
from statistics import median

PATHS = ("ROUTER_NEWO", "ROUTER_NEWO2", "NEWO2_NEWO")


def f(value: str | None) -> float | None:
    if value is None or value == "":
        return None
    return float(value)


def pearson(xs: list[float], ys: list[float]) -> float | None:
    if len(xs) < 3 or len(xs) != len(ys):
        return None
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    dx = [x - mx for x in xs]
    dy = [y - my for y in ys]
    den = math.sqrt(sum(x*x for x in dx) * sum(y*y for y in dy))
    if den <= 1e-12:
        return None
    return sum(x*y for x, y in zip(dx, dy)) / den


def local_dt(text: str) -> datetime:
    return datetime.fromisoformat(text)


def iso(dt: datetime) -> str:
    return dt.isoformat(timespec="seconds")


def merged_runs(rows: list[dict], predicate, window_seconds: float, max_gap_windows: int = 0):
    hits = [i for i, row in enumerate(rows) if predicate(row)]
    if not hits:
        return []
    groups = []
    start = prev = hits[0]
    for idx in hits[1:]:
        if idx - prev <= max_gap_windows + 1:
            prev = idx
            continue
        groups.append((start, prev))
        start = prev = idx
    groups.append((start, prev))
    result = []
    for a, b in groups:
        start_dt = local_dt(rows[a]["local_time"])
        end_dt = local_dt(rows[b]["local_time"]) + timedelta(seconds=window_seconds)
        vals = [f(rows[i].get("consensus_z")) for i in range(a, b+1)]
        vals = [v for v in vals if v is not None]
        result.append({
            "start": iso(start_dt),
            "end": iso(end_dt),
            "duration_seconds": round((end_dt-start_dt).total_seconds(), 1),
            "peak_consensus_z": None if not vals else round(max(vals), 3),
            "median_consensus_z": None if not vals else round(median(vals), 3),
            "start_window": a,
            "end_window": b,
        })
    return result


def event_signature(row: dict) -> dict:
    zs = {path: f(row.get(f"{path}_z")) for path in PATHS}
    available = {k:v for k,v in zs.items() if v is not None}
    if not available:
        return {"path_z": zs, "dominant_path": None, "agreement": "NO_DATA"}
    dominant = max(available, key=available.get)
    above = sum(v > 0.75 for v in available.values())
    high = sum(v > 2.0 for v in available.values())
    if high >= 2:
        agreement = "MULTI_PATH_HIGH"
    elif above >= 2:
        agreement = "MULTI_PATH_MOTION"
    elif high == 1 or above == 1:
        agreement = "SINGLE_PATH_DOMINANT"
    else:
        agreement = "WEAK_OR_MIXED"
    return {
        "path_z": {k:(None if v is None else round(v,3)) for k,v in zs.items()},
        "dominant_path": dominant,
        "agreement": agreement,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv_path")
    ap.add_argument("--motion-summary", default=None)
    ap.add_argument("--output", default="overnight-deep-summary.json")
    ap.add_argument("--minute-output", default="overnight-motion-minute.csv")
    args = ap.parse_args()

    with Path(args.csv_path).open(newline="", encoding="utf-8") as h:
        rows = list(csv.DictReader(h))
    if not rows:
        raise SystemExit("motion CSV is empty")

    if len(rows) >= 2:
        window_seconds = (local_dt(rows[1]["local_time"]) - local_dt(rows[0]["local_time"])).total_seconds()
    else:
        window_seconds = 10.0

    state_counts = {}
    for row in rows:
        state = row.get("relative_state", "UNKNOWN")
        state_counts[state] = state_counts.get(state, 0) + 1

    total_seconds = len(rows) * window_seconds
    state_seconds = {k: round(v*window_seconds, 1) for k,v in state_counts.items()}

    # High-motion bursts: bridge a single 10 s gap to avoid fragmenting one movement.
    high_bursts = merged_runs(rows, lambda r: (f(r.get("consensus_z")) or -999) > 2.0,
                             window_seconds, max_gap_windows=1)
    for burst in high_bursts:
        a, b = burst["start_window"], burst["end_window"]
        peak_i = max(range(a, b+1), key=lambda i: f(rows[i].get("consensus_z")) or -999)
        burst["peak_time"] = rows[peak_i]["local_time"]
        burst["peak_signature"] = event_signature(rows[peak_i])

    low_runs = merged_runs(rows, lambda r: (f(r.get("consensus_z")) is not None and
                                             f(r.get("consensus_z")) <= 0.75),
                           window_seconds, max_gap_windows=0)
    quiet_runs = merged_runs(rows, lambda r: (f(r.get("consensus_z")) is not None and
                                               f(r.get("consensus_z")) <= -0.5),
                             window_seconds, max_gap_windows=0)

    # Longest truly continuous low/quiet stretches.
    low_longest = sorted(low_runs, key=lambda x: x["duration_seconds"], reverse=True)[:10]
    quiet_longest = sorted(quiet_runs, key=lambda x: x["duration_seconds"], reverse=True)[:10]

    # Top 25 10-second events with individual path responses.
    ranked = []
    for row in rows:
        z = f(row.get("consensus_z"))
        if z is None:
            continue
        ranked.append((z, row))
    ranked.sort(key=lambda pair: pair[0], reverse=True)
    top_events = []
    for z, row in ranked[:25]:
        top_events.append({
            "time": row["local_time"],
            "consensus_z": round(z, 3),
            **event_signature(row),
        })

    # Pearson correlation between path z scores over matching windows.
    correlations = {}
    for ai, a in enumerate(PATHS):
        for b in PATHS[ai+1:]:
            xs, ys = [], []
            for row in rows:
                x = f(row.get(f"{a}_z")); y = f(row.get(f"{b}_z"))
                if x is not None and y is not None:
                    xs.append(x); ys.append(y)
            corr = pearson(xs, ys)
            correlations[f"{a}__{b}"] = None if corr is None else round(corr, 4)

    candidate = None
    if args.motion_summary and Path(args.motion_summary).is_file():
        summary = json.loads(Path(args.motion_summary).read_text(encoding="utf-8"))
        candidate = summary.get("candidate_sustained_low_motion_local_time")

    # Minute-level medians for easier plotting / human review.
    minute_rows = []
    bucket = []
    current_minute = None
    def flush_minute():
        if not bucket:
            return
        out = {"minute": current_minute}
        vals = [f(r.get("consensus_z")) for r in bucket]
        vals = [v for v in vals if v is not None]
        out["consensus_z_median"] = "" if not vals else round(median(vals), 6)
        out["consensus_z_max"] = "" if not vals else round(max(vals), 6)
        for path in PATHS:
            pv = [f(r.get(f"{path}_z")) for r in bucket]
            pv = [v for v in pv if v is not None]
            out[f"{path}_z_median"] = "" if not pv else round(median(pv), 6)
            out[f"{path}_z_max"] = "" if not pv else round(max(pv), 6)
        minute_rows.append(out)
    for row in rows:
        minute = local_dt(row["local_time"]).replace(second=0, microsecond=0).isoformat(timespec="minutes")
        if current_minute is None:
            current_minute = minute
        if minute != current_minute:
            flush_minute(); bucket = []; current_minute = minute
        bucket.append(row)
    flush_minute()

    fields = list(minute_rows[0].keys()) if minute_rows else []
    with Path(args.minute_output).open("w", newline="", encoding="utf-8") as h:
        w = csv.DictWriter(h, fieldnames=fields); w.writeheader(); w.writerows(minute_rows)

    result = {
        "source_csv": args.csv_path,
        "first_time": rows[0]["local_time"],
        "last_time": rows[-1]["local_time"],
        "window_seconds": window_seconds,
        "total_windows": len(rows),
        "duration_hours": round(total_seconds/3600, 4),
        "state_seconds": state_seconds,
        "state_percent": {k: round(v*window_seconds/total_seconds*100, 2) for k,v in state_counts.items()},
        "sustained_low_motion_candidate": candidate,
        "high_motion_bursts": high_bursts,
        "longest_low_or_quiet_runs": low_longest,
        "longest_quiet_runs": quiet_longest,
        "top_10s_events": top_events,
        "path_z_correlations": correlations,
        "minute_output": args.minute_output,
        "interpretation_boundary": "relative RF room motion only; not sleep-stage, identity, or person attribution",
    }
    Path(args.output).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    print("=== DEEP MOTION ANALYSIS ===")
    print(f"time: {rows[0]['local_time']} -> {rows[-1]['local_time']}")
    print("state %:", result["state_percent"])
    print("candidate:", candidate)
    print("path correlations:", correlations)
    print("\nHigh-motion bursts:")
    for burst in high_bursts[:15]:
        sig = burst.get("peak_signature", {})
        print(f"{burst['start']} -> {burst['end']}  peak={burst['peak_consensus_z']} "
              f"at {burst.get('peak_time')} {sig.get('agreement')} dominant={sig.get('dominant_path')}")
    print("\nLongest LOW/QUIET stretches:")
    for run in low_longest[:10]:
        print(f"{run['start']} -> {run['end']}  {run['duration_seconds']/60:.1f} min")
    print("\nTop event path signatures:")
    for event in top_events[:12]:
        print(event["time"], "z=", event["consensus_z"], event["agreement"], event["path_z"])
    print("\nJSON:", args.output)
    print("minute CSV:", args.minute_output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
