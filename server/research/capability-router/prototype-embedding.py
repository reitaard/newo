import argparse
import json
import sys
import time
from collections import defaultdict
from pathlib import Path

import numpy as np
from fastembed import TextEmbedding

DEFAULT_MODEL = "sentence-transformers/all-MiniLM-L6-v2"


def read_jsonl(path: Path):
    rows = []
    with path.open("r", encoding="utf-8") as fh:
        for line_no, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"{path}:{line_no}: invalid JSON") from exc
    return rows


def normalize(vec):
    arr = np.asarray(vec, dtype=np.float32)
    norm = np.linalg.norm(arr)
    return arr if norm == 0 else arr / norm


def source_need(capability, registry):
    meta = registry.get(capability, {})
    family = str(meta.get("family", ""))
    if family.startswith("internal") or meta.get("source") in {"memory", "device", "sensor", "camera"}:
        return "internal"
    return "live" if meta.get("live") else "stable"


def main():
    parser = argparse.ArgumentParser(description="Prototype embedding capability router benchmark")
    parser.add_argument("corpus", nargs="?", default="corpus.jsonl")
    parser.add_argument("output", nargs="?", default="prototype-predictions.jsonl")
    parser.add_argument("--prototypes", default="prototype-exemplars.jsonl")
    parser.add_argument("--capabilities", default="capabilities.json")
    parser.add_argument("--model", default=DEFAULT_MODEL)
    args = parser.parse_args()

    corpus_path = Path(args.corpus)
    output_path = Path(args.output)
    prototypes_path = Path(args.prototypes)
    capabilities_path = Path(args.capabilities)

    corpus = read_jsonl(corpus_path)
    prototypes = read_jsonl(prototypes_path)
    capabilities_doc = json.loads(capabilities_path.read_text(encoding="utf-8"))
    registry = {row["id"]: row for row in capabilities_doc["capabilities"]}

    load_started = time.perf_counter()
    model = TextEmbedding(model_name=args.model)
    load_ms = (time.perf_counter() - load_started) * 1000

    prototype_started = time.perf_counter()
    prototype_vectors = list(model.embed([row["text"] for row in prototypes]))
    grouped = defaultdict(list)
    for row, vec in zip(prototypes, prototype_vectors):
        grouped[row["capability"]].append(normalize(vec))

    centroids = {}
    for capability, vectors in grouped.items():
        centroids[capability] = normalize(np.mean(np.stack(vectors), axis=0))
    prototype_ms = (time.perf_counter() - prototype_started) * 1000

    missing = [cap for cap in registry if cap not in centroids]
    if missing:
        raise RuntimeError(f"missing prototype classes: {', '.join(missing)}")

    # Warm one request so per-turn latency excludes one-time runtime initialization.
    warm_started = time.perf_counter()
    list(model.embed(["warm up the capability router"]))
    warmup_ms = (time.perf_counter() - warm_started) * 1000

    labels = list(centroids)
    centroid_matrix = np.stack([centroids[label] for label in labels])
    output_rows = []

    for row in corpus:
        started = time.perf_counter()
        query_vec = normalize(next(iter(model.embed([row["text"]]))))
        sims = centroid_matrix @ query_vec
        ranked_idx = np.argsort(-sims)
        primary = labels[int(ranked_idx[0])]
        latency_ms = (time.perf_counter() - started) * 1000
        scores = {label: float(score) for label, score in zip(labels, sims)}
        output_rows.append({
            "id": row["id"],
            "primary": primary,
            "capabilities": [primary],
            "scores": scores,
            "source_need": source_need(primary, registry),
            "abstain": False,
            "latency_ms": latency_ms,
        })

    with output_path.open("w", encoding="utf-8", newline="\n") as fh:
        for row in output_rows:
            fh.write(json.dumps(row, separators=(",", ":")) + "\n")

    print(
        f"model={args.model} rows={len(corpus)} classes={len(labels)} "
        f"load_ms={load_ms:.1f} prototype_ms={prototype_ms:.1f} warmup_ms={warmup_ms:.1f}",
        file=sys.stderr,
    )
    print(f"wrote {len(output_rows)} predictions to {output_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
