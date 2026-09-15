#!/usr/bin/env python3
"""Benchmark Liquid Prompt Router quality and latency against Newo semantic lanes."""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import statistics
import time
from collections import Counter
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parents[1]
DEFAULT_ROUTES = SERVER_DIR / "config" / "intelligence-routes.json"
DEFAULT_CORPUS = SERVER_DIR / "config" / "intelligence-router-benchmark.json"


def percentile(values: list[float], q: float) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(q * len(ordered)) - 1))
    return ordered[index]


def load_routes(path: Path) -> tuple[dict, list[dict]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    routes = document.get("routes")
    if not isinstance(routes, list) or len(routes) < 2:
        raise ValueError("routes file must contain at least two routes")

    ids = [item.get("id") for item in routes]
    labels = [item.get("route") for item in routes]
    if not all(isinstance(value, str) and value.strip() for value in ids + labels):
        raise ValueError("every route needs non-empty id and route strings")
    if len(ids) != len(set(ids)) or len(labels) != len(set(labels)):
        raise ValueError("route ids and route labels must be unique")
    return document, routes


def load_cases(path: Path, valid_ids: set[str]) -> list[dict]:
    document = json.loads(path.read_text(encoding="utf-8"))
    cases = document.get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError("corpus file must contain cases")

    for index, case in enumerate(cases):
        expected = case.get("expected")
        text = case.get("text")
        if expected not in valid_ids:
            raise ValueError(f"case {index} has unknown expected route: {expected!r}")
        if not isinstance(text, str) or not text.strip():
            raise ValueError(f"case {index} has empty text")
    return cases


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Measure Newo Liquid Prompt Router accuracy, confusions, margins, and warm latency."
    )
    parser.add_argument("--routes", type=Path, default=DEFAULT_ROUTES)
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    parser.add_argument("--model", help="Hugging Face model id or local model directory; defaults to routes JSON")
    parser.add_argument("--threads", type=int, default=2, help="PyTorch CPU threads (default: 2)")
    parser.add_argument("--warmup", type=int, default=3, help="Warm-up route calls before timing")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--limit", type=int, default=0, help="Run only N shuffled cases; 0 runs all")
    parser.add_argument("--show-all", action="store_true", help="Print every prediction instead of failures only")
    parser.add_argument("--local-files-only", action="store_true", help="Do not access Hugging Face network")
    parser.add_argument("--json-out", type=Path, help="Optional path for machine-readable results")
    args = parser.parse_args()

    if args.threads < 1:
        parser.error("--threads must be >= 1")
    if args.warmup < 0:
        parser.error("--warmup must be >= 0")
    if args.limit < 0:
        parser.error("--limit must be >= 0")

    routes_document, route_defs = load_routes(args.routes)
    route_ids = {item["id"] for item in route_defs}
    cases = load_cases(args.corpus, route_ids)
    route_labels = [item["route"] for item in route_defs]
    id_by_label = {item["route"]: item["id"] for item in route_defs}
    label_by_id = {item["id"]: item["route"] for item in route_defs}

    rng = random.Random(args.seed)
    cases = list(cases)
    rng.shuffle(cases)
    if args.limit:
        cases = cases[: args.limit]

    model_id = args.model or routes_document.get("router")
    if not model_id:
        raise ValueError("no model supplied and routes JSON has no router field")

    # This benchmark is deliberately CPU-only because that is the intended VPS deployment.
    os.environ["CUDA_VISIBLE_DEVICES"] = ""
    os.environ.setdefault("TOKENIZERS_PARALLELISM", "false")

    import torch
    from transformers import AutoModel, AutoTokenizer

    torch.set_num_threads(args.threads)
    try:
        torch.set_num_interop_threads(1)
    except RuntimeError:
        pass

    print(f"model: {model_id}")
    print(f"routes: {len(route_defs)}")
    print(f"cases: {len(cases)}")
    print(f"cpu_threads: {args.threads}")

    load_started = time.perf_counter()
    tokenizer = AutoTokenizer.from_pretrained(
        model_id,
        trust_remote_code=True,
        local_files_only=args.local_files_only,
    )
    model = AutoModel.from_pretrained(
        model_id,
        trust_remote_code=True,
        local_files_only=args.local_files_only,
    ).eval().to("cpu")
    load_ms = (time.perf_counter() - load_started) * 1000.0
    print(f"load_ms: {load_ms:.1f}")

    warm_text = cases[0]["text"] if cases else "Hello"
    for _ in range(args.warmup):
        model.route(warm_text, route_labels, tokenizer=tokenizer)

    latencies = []
    margins = []
    correct_margins = []
    wrong_margins = []
    per_lane_total = Counter()
    per_lane_correct = Counter()
    confusion = Counter()
    results = []

    for number, case in enumerate(cases, start=1):
        text = case["text"]
        expected = case["expected"]
        started = time.perf_counter_ns()
        ranked = model.route(text, route_labels, tokenizer=tokenizer)
        latency_ms = (time.perf_counter_ns() - started) / 1_000_000.0

        if not ranked:
            raise RuntimeError("model.route() returned no routes")
        predicted_label = ranked[0]["route"]
        if predicted_label not in id_by_label:
            raise RuntimeError(f"model returned unknown route label: {predicted_label!r}")

        predicted = id_by_label[predicted_label]
        top1 = float(ranked[0]["score"])
        top2 = float(ranked[1]["score"]) if len(ranked) > 1 else 0.0
        margin = top1 - top2
        ok = predicted == expected

        latencies.append(latency_ms)
        margins.append(margin)
        (correct_margins if ok else wrong_margins).append(margin)
        per_lane_total[expected] += 1
        if ok:
            per_lane_correct[expected] += 1
        else:
            confusion[(expected, predicted)] += 1

        record = {
            "text": text,
            "expected": expected,
            "predicted": predicted,
            "correct": ok,
            "score": top1,
            "margin": margin,
            "latency_ms": latency_ms,
            "tags": case.get("tags", []),
            "top2": [
                {"id": id_by_label[item["route"]], "score": float(item["score"])}
                for item in ranked[:2]
            ],
        }
        results.append(record)

        if args.show_all or not ok:
            marker = "OK" if ok else "MISS"
            print(
                f"[{marker:4}] {number:03d} expected={expected:<23} "
                f"predicted={predicted:<23} score={top1:.3f} "
                f"margin={margin:.3f} latency={latency_ms:.1f}ms"
            )
            print(f"       {text}")

    total = len(results)
    correct = sum(1 for item in results if item["correct"])
    accuracy = correct / total if total else 0.0
    boundary = [item for item in results if "boundary" in item["tags"]]
    boundary_correct = sum(1 for item in boundary if item["correct"])
    boundary_accuracy = boundary_correct / len(boundary) if boundary else None

    print("\n=== SUMMARY ===")
    print(f"accuracy: {correct}/{total} = {accuracy * 100:.1f}%")
    if boundary_accuracy is not None:
        print(f"boundary_accuracy: {boundary_correct}/{len(boundary)} = {boundary_accuracy * 100:.1f}%")
    print(
        "latency_ms: "
        f"mean={statistics.fmean(latencies):.1f} "
        f"p50={percentile(latencies, 0.50):.1f} "
        f"p95={percentile(latencies, 0.95):.1f} "
        f"max={max(latencies):.1f}"
    )
    print(
        "margin: "
        f"p50={percentile(margins, 0.50):.3f} "
        f"correct_p50={percentile(correct_margins, 0.50):.3f} "
        f"wrong_p50={percentile(wrong_margins, 0.50):.3f}"
    )

    print("\n=== PER LANE ===")
    for route in route_defs:
        lane = route["id"]
        lane_total = per_lane_total[lane]
        lane_correct = per_lane_correct[lane]
        pct = (lane_correct / lane_total * 100.0) if lane_total else 0.0
        print(f"{lane:<23} {lane_correct:>2}/{lane_total:<2} {pct:>6.1f}%  ({label_by_id[lane]})")

    print("\n=== CONFUSIONS ===")
    if not confusion:
        print("none")
    else:
        for (expected, predicted), count in sorted(confusion.items(), key=lambda item: (-item[1], item[0])):
            print(f"{count:>2}  {expected} -> {predicted}")

    summary = {
        "model": model_id,
        "routes_file": str(args.routes),
        "corpus_file": str(args.corpus),
        "seed": args.seed,
        "threads": args.threads,
        "warmup": args.warmup,
        "load_ms": load_ms,
        "cases": total,
        "correct": correct,
        "accuracy": accuracy,
        "boundary_cases": len(boundary),
        "boundary_correct": boundary_correct,
        "boundary_accuracy": boundary_accuracy,
        "latency_ms": {
            "mean": statistics.fmean(latencies),
            "p50": percentile(latencies, 0.50),
            "p95": percentile(latencies, 0.95),
            "max": max(latencies),
        },
        "margin": {
            "p50": percentile(margins, 0.50),
            "correct_p50": percentile(correct_margins, 0.50),
            "wrong_p50": percentile(wrong_margins, 0.50),
        },
        "per_lane": {
            route["id"]: {
                "correct": per_lane_correct[route["id"]],
                "total": per_lane_total[route["id"]],
                "accuracy": (
                    per_lane_correct[route["id"]] / per_lane_total[route["id"]]
                    if per_lane_total[route["id"]]
                    else 0.0
                ),
            }
            for route in route_defs
        },
        "confusions": [
            {"expected": expected, "predicted": predicted, "count": count}
            for (expected, predicted), count in sorted(confusion.items(), key=lambda item: (-item[1], item[0]))
        ],
        "results": results,
    }

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(f"\nwrote: {args.json_out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
