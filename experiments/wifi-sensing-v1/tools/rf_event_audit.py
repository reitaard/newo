"""Audit a CSI event window for RF/packet-metadata changes.

This is a nuisance-analysis tool, not a human-motion classifier. It compares a
short event window against nearby baseline CSI and reports per-path RSSI, noise,
rate/MCS, packet length, aggregation, and geometry distributions so apparent
motion events can be checked for radio/packet artifacts.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path
from statistics import median

from newo_csi.archive import iter_archive
from newo_csi.protocol import CsiRecord, PATH_NAMES, decode


def pct(vals, q):
    if not vals:
        return None
    s = sorted(vals)
    x = (len(s)-1)*q
    lo = int(x); hi = min(len(s)-1, lo+1)
    f = x-lo
    return s[lo]*(1-f)+s[hi]*f


def top(c: Counter, n=8):
    return c.most_common(n)


def stats(vals):
    if not vals:
        return None
    return {
        "n": len(vals),
        "min": min(vals),
        "p10": round(pct(vals, .10), 3),
        "median": round(median(vals), 3),
        "p90": round(pct(vals, .90), 3),
        "max": max(vals),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("session")
    ap.add_argument("--time", required=True, help="event start ISO time with offset")
    ap.add_argument("--event-seconds", type=float, default=10.0)
    ap.add_argument("--baseline-seconds", type=float, default=120.0,
                    help="seconds before and after event used as baseline")
    args = ap.parse_args()

    p = Path(args.session)
    archive = p / "frames.ncsi" if p.is_dir() else p
    t0 = datetime.fromisoformat(args.time).timestamp()
    t1 = t0 + args.event_seconds
    b0 = t0 - args.baseline_seconds
    b1 = t1 + args.baseline_seconds

    buckets = {"baseline": defaultdict(lambda: defaultdict(list)),
               "event": defaultdict(lambda: defaultdict(list))}
    counters = {"baseline": defaultdict(lambda: defaultdict(Counter)),
                "event": defaultdict(lambda: defaultdict(Counter))}

    total = 0
    for item in iter_archive(archive):
        total += 1
        ts = item.host_wall_ns / 1e9
        if ts < b0 or ts > b1:
            continue
        rec = decode(item.record)
        if not isinstance(rec, CsiRecord):
            continue
        region = "event" if t0 <= ts < t1 else "baseline"
        path = PATH_NAMES.get(rec.path_id, f"PATH_{rec.path_id}")
        vals = buckets[region][path]
        vals["rssi"].append(rec.rssi_dbm)
        vals["noise"].append(rec.noise_floor_dbm)
        cnt = counters[region][path]
        cnt["geometry"][(rec.csi_payload_length, rec.bandwidth, rec.phy_mode, rec.ltf_mask)] += 1
        cnt["mcs"][rec.mcs] += 1
        cnt["phy_rate"][rec.phy_rate] += 1
        cnt["packet_length"][rec.packet_length] += 1
        cnt["ampdu_count"][rec.ampdu_count] += 1
        cnt["rx_flags"][rec.rx_flags] += 1
        cnt["rx_state"][rec.rx_state] += 1

    print(f"event: {args.time} for {args.event_seconds:.1f}s")
    print(f"baseline: {args.baseline_seconds:.1f}s before + after (excluding event)\n")

    all_paths = sorted(set(buckets["baseline"]) | set(buckets["event"]))
    for path in all_paths:
        print(f"=== {path} ===")
        for region in ("baseline", "event"):
            vals = buckets[region][path]
            cnt = counters[region][path]
            n = len(vals.get("rssi", []))
            print(region, "frames=", n)
            print("  RSSI:", stats(vals.get("rssi", [])))
            print("  noise:", stats(vals.get("noise", [])))
            print("  geometry:", top(cnt["geometry"]))
            print("  MCS:", top(cnt["mcs"]))
            print("  phy_rate:", top(cnt["phy_rate"]))
            print("  packet_length:", top(cnt["packet_length"]))
            print("  ampdu_count:", top(cnt["ampdu_count"]))
            print("  rx_flags:", top(cnt["rx_flags"]))
            print("  rx_state:", top(cnt["rx_state"]))
        print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
