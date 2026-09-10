"""Streaming, geometry-aware coarse motion decoder for archived Newo CSI.

This is an inspection/feature-extraction tool, not a sleep detector or gesture
classifier. It converts raw CSI into relative 10 s motion windows while keeping
paths and CSI PHY/LTF geometries separate.
"""

from __future__ import annotations

import argparse
import csv
from collections import Counter, defaultdict
from datetime import datetime
import json
import math
from pathlib import Path
from statistics import median

from newo_csi.archive import iter_archive
from newo_csi.protocol import CsiRecord, PATH_NAMES, decode


def percentile(values: list[float], q: float) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    pos = (len(ordered) - 1) * q
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return ordered[lo]
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def selected_indices(count: int, wanted: int = 24) -> tuple[int, ...]:
    # Avoid edge bins and the sanitized leading bytes. Spread the selected bins
    # across the usable geometry instead of assuming one fixed subcarrier map.
    edge = max(4, count // 20)
    start = edge
    stop = count - edge - 1
    if stop <= start:
        return tuple(range(count))
    n = min(wanted, stop - start + 1)
    if n <= 1:
        return (start,)
    return tuple(sorted({round(start + i * (stop - start) / (n - 1)) for i in range(n)}))


def amplitude_shape(record: CsiRecord, indices: tuple[int, ...]) -> tuple[float, ...] | None:
    payload = record.iq_bytes
    values: list[float] = []
    for index in indices:
        offset = index * 2
        imag = payload[offset]
        real = payload[offset + 1]
        if imag >= 128:
            imag -= 256
        if real >= 128:
            real -= 256
        values.append(math.hypot(real, imag))
    norm = math.sqrt(sum(value * value for value in values))
    if norm <= 1e-12:
        return None
    return tuple(value / norm for value in values)


def shape_distance(left: tuple[float, ...], right: tuple[float, ...]) -> float:
    # Unit-normalized amplitude vectors: cosine distance rejects much of the
    # frame-wide gain/RSSI movement while retaining changes in CSI shape.
    dot = sum(a * b for a, b in zip(left, right))
    return max(0.0, min(2.0, 1.0 - dot))


def local_iso(wall_ns: int) -> str:
    return datetime.fromtimestamp(wall_ns / 1e9).astimezone().isoformat(timespec="seconds")


def classify(z: float | None) -> str:
    if z is None:
        return "NO_DATA"
    if z <= -0.5:
        return "QUIET"
    if z <= 0.75:
        return "LOW"
    if z <= 2.0:
        return "MOTION"
    return "HIGH_MOTION"


def main() -> int:
    parser = argparse.ArgumentParser(description="derive a coarse motion timeline from frames.ncsi")
    parser.add_argument("session", help="dataset directory or frames.ncsi path")
    parser.add_argument("--window-seconds", type=float, default=10.0)
    parser.add_argument("--sample-hz", type=float, default=10.0,
                        help="maximum analyzed CSI frames per geometry/path per second")
    parser.add_argument("--output", default="motion-windows.csv")
    parser.add_argument("--summary", default="motion-summary.json")
    args = parser.parse_args()

    session = Path(args.session)
    archive_path = session / "frames.ncsi" if session.is_dir() else session
    if not archive_path.is_file():
        raise SystemExit(f"archive not found: {archive_path}")
    if args.window_seconds <= 0 or args.sample_hz <= 0:
        raise SystemExit("window-seconds and sample-hz must be positive")

    window_ns = int(args.window_seconds * 1e9)
    sample_interval_ns = int(1e9 / args.sample_hz)

    first_mono: int | None = None
    first_wall: int | None = None
    last_mono: int | None = None
    last_wall: int | None = None
    total_records = 0
    csi_records = 0

    # Geometry key is deliberately stricter than CSI byte length alone.
    # (path, payload length, bandwidth, PHY mode, LTF mask)
    geometry_counts: Counter[tuple[int, int, int, int, int]] = Counter()
    indices_cache: dict[tuple[int, int, int, int, int], tuple[int, ...]] = {}
    last_sample_ns: dict[tuple[int, int, int, int, int], int] = {}
    previous_shape: dict[tuple[int, int, int, int, int], tuple[float, ...]] = {}
    window_deltas: dict[tuple[int, tuple[int, int, int, int, int]], list[float]] = defaultdict(list)
    window_rssi: dict[tuple[int, tuple[int, int, int, int, int]], list[int]] = defaultdict(list)

    for item in iter_archive(archive_path):
        total_records += 1
        if first_mono is None:
            first_mono = item.host_monotonic_ns
            first_wall = item.host_wall_ns
        last_mono = item.host_monotonic_ns
        last_wall = item.host_wall_ns

        record = decode(item.record)
        if not isinstance(record, CsiRecord):
            if total_records % 200000 == 0:
                print(f"{total_records:,} records scanned...")
            continue

        csi_records += 1
        key = (record.path_id, record.csi_payload_length, record.bandwidth,
               record.phy_mode, record.ltf_mask)

        previous_ns = last_sample_ns.get(key)
        if previous_ns is not None and item.host_monotonic_ns - previous_ns < sample_interval_ns:
            if total_records % 200000 == 0:
                print(f"{total_records:,} records scanned...")
            continue

        last_sample_ns[key] = item.host_monotonic_ns
        geometry_counts[key] += 1
        indices = indices_cache.setdefault(key, selected_indices(record.subcarrier_item_count))
        shape = amplitude_shape(record, indices)
        if shape is None:
            continue

        previous = previous_shape.get(key)
        previous_shape[key] = shape
        if previous is None:
            continue
        # Do not compare shapes across a long packet gap.
        if previous_ns is not None and item.host_monotonic_ns - previous_ns > sample_interval_ns * 8:
            continue

        assert first_mono is not None
        window_index = int((item.host_monotonic_ns - first_mono) // window_ns)
        bucket = (window_index, key)
        window_deltas[bucket].append(shape_distance(previous, shape))
        window_rssi[bucket].append(record.rssi_dbm)

        if total_records % 200000 == 0:
            print(f"{total_records:,} records scanned...")

    if first_mono is None or first_wall is None or last_mono is None or last_wall is None:
        raise SystemExit("archive contains no records")

    # Pick the most frequently sampled stable geometry independently per path.
    dominant: dict[int, tuple[int, int, int, int, int]] = {}
    for key, count in geometry_counts.items():
        path = key[0]
        current = dominant.get(path)
        if current is None or geometry_counts[current] < count:
            dominant[path] = key

    raw_scores: dict[int, dict[int, float]] = defaultdict(dict)
    raw_p90: dict[int, dict[int, float]] = defaultdict(dict)
    sample_counts: dict[int, dict[int, int]] = defaultdict(dict)
    mean_rssi: dict[int, dict[int, float]] = defaultdict(dict)

    for path, key in dominant.items():
        for (window_index, bucket_key), values in window_deltas.items():
            if bucket_key != key or not values:
                continue
            raw_scores[path][window_index] = median(values)
            raw_p90[path][window_index] = percentile(values, 0.90)
            sample_counts[path][window_index] = len(values)
            rssis = window_rssi[(window_index, key)]
            if rssis:
                mean_rssi[path][window_index] = sum(rssis) / len(rssis)

    baselines: dict[int, dict[str, float]] = {}
    zscores: dict[int, dict[int, float]] = defaultdict(dict)
    for path, scores_by_window in raw_scores.items():
        values = list(scores_by_window.values())
        if not values:
            continue
        center = median(values)
        mad = median([abs(value - center) for value in values])
        scale = max(1e-12, 1.4826 * mad)
        baselines[path] = {"median": center, "mad": mad, "robust_scale": scale}
        for window_index, value in scores_by_window.items():
            zscores[path][window_index] = (value - center) / scale

    max_window = int((last_mono - first_mono) // window_ns)
    rows: list[dict[str, object]] = []
    consensus_by_window: dict[int, float] = {}
    for window_index in range(max_window + 1):
        wall_ns = first_wall + window_index * window_ns
        z_values = [zscores[path][window_index] for path in sorted(zscores)
                    if window_index in zscores[path]]
        consensus = median(z_values) if z_values else None
        if consensus is not None:
            consensus_by_window[window_index] = consensus
        row: dict[str, object] = {
            "window": window_index,
            "local_time": local_iso(wall_ns),
            "consensus_z": "" if consensus is None else round(consensus, 6),
            "relative_state": classify(consensus),
        }
        for path in (1, 2, 3):
            name = PATH_NAMES[path]
            row[f"{name}_score"] = "" if window_index not in raw_scores[path] else round(raw_scores[path][window_index], 9)
            row[f"{name}_p90"] = "" if window_index not in raw_p90[path] else round(raw_p90[path][window_index], 9)
            row[f"{name}_z"] = "" if window_index not in zscores[path] else round(zscores[path][window_index], 6)
            row[f"{name}_samples"] = sample_counts[path].get(window_index, 0)
            row[f"{name}_rssi"] = "" if window_index not in mean_rssi[path] else round(mean_rssi[path][window_index], 2)
        rows.append(row)

    fieldnames = list(rows[0].keys()) if rows else []
    with Path(args.output).open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    # Conservative candidate: first point after 5 min with 1 min continuously
    # below the per-session median and >=70% of the following 10 min no higher
    # than LOW. This is a low-motion candidate, never an inferred sleep onset.
    candidate: int | None = None
    consecutive = max(1, round(60 / args.window_seconds))
    future = max(consecutive, round(600 / args.window_seconds))
    start_after = max(0, round(300 / args.window_seconds))
    for index in range(start_after, max_window - future + 1):
        first_block = [consensus_by_window.get(i) for i in range(index, index + consecutive)]
        if any(value is None or value > 0.0 for value in first_block):
            continue
        lookahead = [consensus_by_window.get(i) for i in range(index, index + future)]
        available = [value for value in lookahead if value is not None]
        if available and sum(value <= 0.75 for value in available) / len(available) >= 0.70:
            candidate = index
            break

    geometry_summary = {}
    for path, key in dominant.items():
        geometry_summary[PATH_NAMES.get(path, str(path))] = {
            "path_id": path,
            "payload_length": key[1],
            "bandwidth": key[2],
            "phy_mode": key[3],
            "ltf_mask": key[4],
            "sampled_frames": geometry_counts[key],
        }

    summary = {
        "archive": str(archive_path),
        "records_scanned": total_records,
        "csi_records": csi_records,
        "first_local_time": local_iso(first_wall),
        "last_local_time": local_iso(last_wall),
        "duration_hours": round((last_mono - first_mono) / 1e9 / 3600, 6),
        "window_seconds": args.window_seconds,
        "sample_hz_cap": args.sample_hz,
        "dominant_geometry": geometry_summary,
        "path_robust_baselines": {PATH_NAMES.get(path, str(path)): value
                                  for path, value in baselines.items()},
        "candidate_sustained_low_motion_local_time": (
            None if candidate is None else local_iso(first_wall + candidate * window_ns)
        ),
        "candidate_note": "relative low-motion candidate only; not biological sleep onset",
        "output_csv": args.output,
    }
    Path(args.summary).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print("\n=== MOTION DECODE ===")
    print(f"records: {total_records:,}  CSI: {csi_records:,}")
    print(f"time: {local_iso(first_wall)} -> {local_iso(last_wall)}")
    for name, geometry in geometry_summary.items():
        print(f"{name}: dominant geometry len={geometry['payload_length']} bw={geometry['bandwidth']} "
              f"phy={geometry['phy_mode']} ltf={geometry['ltf_mask']} sampled={geometry['sampled_frames']:,}")
    if candidate is None:
        print("sustained-low-motion candidate: not found by conservative rule")
    else:
        print("sustained-low-motion candidate:", local_iso(first_wall + candidate * window_ns))
    print("NOTE: candidate is relative room motion, not inferred sleep onset.")
    print("CSV:", args.output)
    print("summary:", args.summary)

    active = sorted(consensus_by_window.items(), key=lambda pair: pair[1], reverse=True)[:10]
    print("\nMost active 10 s windows:")
    for window_index, score in active:
        print(local_iso(first_wall + window_index * window_ns), f"z={score:.2f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
