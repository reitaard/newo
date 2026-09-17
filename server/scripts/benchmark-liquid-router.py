#!/usr/bin/env python3
"""Benchmark Newo's conservative shortcuts plus Liquid Prompt Router fallback."""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import re
import statistics
import time
from collections import Counter
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parents[1]
DEFAULT_ROUTES = SERVER_DIR / "config" / "intelligence-routes.json"
DEFAULT_CORPUS = SERVER_DIR / "config" / "intelligence-router-benchmark.json"
DEFAULT_SHORTCUTS = SERVER_DIR / "config" / "intelligence-shortcuts.json"


def percentile(values: list[float], q: float) -> float:
    if not values:
        return float("nan")
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(q * len(ordered)) - 1))
    return ordered[index]


def latency_summary(values: list[float]) -> dict | None:
    if not values:
        return None
    return {
        "mean": statistics.fmean(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "max": max(values),
    }


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


def load_shortcuts(path: Path, valid_ids: set[str]) -> tuple[dict, list[dict]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    rules = document.get("rules")
    if not isinstance(rules, list):
        raise ValueError("shortcuts file must contain a rules list")

    compiled = []
    seen_ids = set()
    for index, rule in enumerate(rules):
        rule_id = rule.get("id")
        route = rule.get("route")
        patterns = rule.get("patterns")
        if not isinstance(rule_id, str) or not rule_id.strip() or rule_id in seen_ids:
            raise ValueError(f"shortcut rule {index} has invalid or duplicate id")
        if route not in valid_ids:
            raise ValueError(f"shortcut rule {rule_id!r} has unknown route: {route!r}")
        if not isinstance(patterns, list) or not patterns or not all(isinstance(item, str) for item in patterns):
            raise ValueError(f"shortcut rule {rule_id!r} must contain regex patterns")
        seen_ids.add(rule_id)
        compiled.append({
            "id": rule_id,
            "route": route,
            "patterns": [(source, re.compile(source, re.IGNORECASE)) for source in patterns],
        })
    return document, compiled


def match_shortcut(text: str, rules: list[dict]) -> dict | None:
    value = " ".join(String for String in str(text).strip().split())
    for rule in rules:
        for source, pattern in rule["patterns"]:
            if pattern.search(value):
                return {"route": rule["route"], "rule": rule["id"], "pattern": source}
    return None


def format_latency(values: list[float]) -> str:
    stats = latency_summary(values)
    if stats is None:
        return "n/a"
    return (
        f"mean={stats['mean']:.1f} p50={stats['p50']:.1f} "
        f"p95={stats['p95']:.1f} max={stats['max']:.1f}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Measure Newo hybrid shortcut + Liquid router quality and warm latency."
    )
    parser.add_argument("--routes", type=Path, default=DEFAULT_ROUTES)
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    parser.add_argument("--shortcuts", type=Path, default=DEFAULT_SHORTCUTS)
    parser.add_argument("--no-shortcuts", action="store_true", help="Disable deterministic shortcuts for raw Liquid A/B")
    parser.add_argument("--model", help="Hugging Face model id or local model directory; defaults to routes JSON")
    parser.add_argument("--threads", type=int, default=2, help="PyTorch CPU threads (default: 2)")
    parser.add_argument("--warmup", type=int, default=3, help="Warm-up Liquid route calls before timing")
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

    shortcuts_document = None
    shortcut_rules = []
    if not args.no_shortcuts:
        shortcuts_document, shortcut_rules = load_shortcuts(args.shortcuts, route_ids)

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
    print(f"shortcuts: {'disabled' if args.no_shortcuts else f'enabled ({len(shortcut_rules)} rules)'}")

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
    liquid_latencies = []
    shortcut_latencies = []
    margins = []
    correct_margins = []
    wrong_margins = []
    per_lane_total = Counter()
    per_lane_correct = Counter()
    confusion = Counter()
    source_total = Counter()
    source_correct = Counter()
    shortcut_rule_total = Counter()
    shortcut_rule_correct = Counter()
    results = []

    for number, case in enumerate(cases, start=1):
        text = case["text"]
        expected = case["expected"]
        started = time.perf_counter_ns()
        shortcut = match_shortcut(text, shortcut_rules) if shortcut_rules else None

        if shortcut is not None:
            predicted = shortcut["route"]
            source = "shortcut"
            score = None
            margin = None
            top2_result = []
            rule_id = shortcut["rule"]
            latency_ms = (time.perf_counter_ns() - started) / 1_000_000.0
            shortcut_latencies.append(latency_ms)
            shortcut_rule_total[rule_id] += 1
        else:
            ranked = model.route(text, route_labels, tokenizer=tokenizer)
            latency_ms = (time.perf_counter_ns() - started) / 1_000_000.0
            if not ranked:
                raise RuntimeError("model.route() returned no routes")
            predicted_label = ranked[0]["route"]
            if predicted_label not in id_by_label:
                raise RuntimeError(f"model returned unknown route label: {predicted_label!r}")
            predicted = id_by_label[predicted_label]
            source = "liquid"
            score = float(ranked[0]["score"])
            runner_up = float(ranked[1]["score"]) if len(ranked) > 1 else 0.0
            margin = score - runner_up
            top2_result = [
                {"id": id_by_label[item["route"]], "score": float(item["score"])}
                for item in ranked[:2]
            ]
            rule_id = None
            liquid_latencies.append(latency_ms)
            margins.append(margin)

        ok = predicted == expected
        latencies.append(latency_ms)
        source_total[source] += 1
        per_lane_total[expected] += 1
        if ok:
            source_correct[source] += 1
            per_lane_correct[expected] += 1
            if rule_id:
                shortcut_rule_correct[rule_id] += 1
        else:
            confusion[(expected, predicted)] += 1

        if margin is not None:
            (correct_margins if ok else wrong_margins).append(margin)

        record = {
            "text": text,
            "expected": expected,
            "predicted": predicted,
            "correct": ok,
            "source": source,
            "shortcut_rule": rule_id,
            "score": score,
            "margin": margin,
            "latency_ms": latency_ms,
            "tags": case.get("tags", []),
            "top2": top2_result,
        }
        results.append(record)

        if args.show_all or not ok:
            marker = "OK" if ok else "MISS"
            score_text = "-" if score is None else f"{score:.3f}"
            margin_text = "-" if margin is None else f"{margin:.3f}"
            source_text = source if rule_id is None else f"shortcut:{rule_id}"
            print(
                f"[{marker:4}] {number:03d} expected={expected:<23} "
                f"predicted={predicted:<23} source={source_text:<34} "
                f"score={score_text:<5} margin={margin_text:<5} latency={latency_ms:.1f}ms"
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
    print(f"end_to_end_latency_ms: {format_latency(latencies)}")
    print(f"liquid_latency_ms: {format_latency(liquid_latencies)}")
    print(f"shortcut_latency_ms: {format_latency(shortcut_latencies)}")
    print(
        f"shortcut_routes: {source_total['shortcut']}/{total} "
        f"correct={source_correct['shortcut']}/{source_total['shortcut']}"
        if source_total["shortcut"] else "shortcut_routes: 0"
    )
    print(
        f"liquid_routes: {source_total['liquid']}/{total} "
        f"correct={source_correct['liquid']}/{source_total['liquid']}"
        if source_total["liquid"] else "liquid_routes: 0"
    )
    if margins:
        print(
            "liquid_margin: "
            f"p50={percentile(margins, 0.50):.3f} "
            f"correct_p50={percentile(correct_margins, 0.50):.3f} "
            f"wrong_p50={percentile(wrong_margins, 0.50):.3f}"
        )

    if shortcut_rule_total:
        print("\n=== SHORTCUT RULES ===")
        for rule in shortcut_rules:
            rule_id = rule["id"]
            count = shortcut_rule_total[rule_id]
            if not count:
                continue
            good = shortcut_rule_correct[rule_id]
            print(f"{rule_id:<28} {good:>2}/{count:<2} {good / count * 100.0:>6.1f}%")

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
        "shortcuts_file": None if args.no_shortcuts else str(args.shortcuts),
        "shortcuts_version": None if shortcuts_document is None else shortcuts_document.get("version"),
        "shortcuts_enabled": not args.no_shortcuts,
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
        "routing_source": {
            "shortcut": {
                "count": source_total["shortcut"],
                "correct": source_correct["shortcut"],
                "accuracy": source_correct["shortcut"] / source_total["shortcut"] if source_total["shortcut"] else None,
            },
            "liquid": {
                "count": source_total["liquid"],
                "correct": source_correct["liquid"],
                "accuracy": source_correct["liquid"] / source_total["liquid"] if source_total["liquid"] else None,
            },
        },
        "latency_ms": latency_summary(latencies),
        "liquid_latency_ms": latency_summary(liquid_latencies),
        "shortcut_latency_ms": latency_summary(shortcut_latencies),
        "margin": {
            "p50": percentile(margins, 0.50) if margins else None,
            "correct_p50": percentile(correct_margins, 0.50) if correct_margins else None,
            "wrong_p50": percentile(wrong_margins, 0.50) if wrong_margins else None,
        },
        "shortcut_rules": {
            rule["id"]: {
                "route": rule["route"],
                "count": shortcut_rule_total[rule["id"]],
                "correct": shortcut_rule_correct[rule["id"]],
                "accuracy": (
                    shortcut_rule_correct[rule["id"]] / shortcut_rule_total[rule["id"]]
                    if shortcut_rule_total[rule["id"]]
                    else None
                ),
            }
            for rule in shortcut_rules
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
