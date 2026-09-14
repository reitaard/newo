import argparse
import json
import sys
import time
from collections import Counter
from pathlib import Path

import joblib
import numpy as np
import torch
from datasets import Dataset
from setfit import SetFitModel, Trainer, TrainingArguments
from sklearn.linear_model import LogisticRegression

DEFAULT_MODEL = "sentence-transformers/all-MiniLM-L6-v2"
ABSTAIN_LABEL = "__abstain__"
MODE_KNOWLEDGE = "knowledge"
MODE_TOOL = "tool"
MODE_ABSTAIN = "abstain"
MODE_HEAD_FILE = "route-mode-head.joblib"
TOOL_HEAD_FILE = "tool-capability-head.joblib"


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


def normalize_text(text):
    return " ".join(str(text).lower().split())


def source_need(capability, registry):
    meta = registry.get(capability, {})
    family = str(meta.get("family", ""))
    if family.startswith("internal") or meta.get("source") in {"memory", "device", "sensor", "camera"}:
        return "internal"
    return "live" if meta.get("live") else "stable"


def choose_training_device(requested):
    if requested != "auto":
        return requested
    return "cuda" if torch.cuda.is_available() else "cpu"


def remove_exact_eval_leakage(train_rows, eval_rows):
    eval_texts = {normalize_text(row.get("text", "")) for row in eval_rows}
    kept = []
    removed = []
    for row in train_rows:
        if normalize_text(row.get("text", "")) in eval_texts:
            removed.append(row)
        else:
            kept.append(row)
    return kept, removed


def validate_training_data(train_rows, labels):
    counts = Counter(row.get("label") for row in train_rows)
    missing = [label for label in labels if not counts.get(label)]
    unknown = sorted(set(counts) - set(labels))
    if missing:
        raise RuntimeError(f"training data missing labels: {', '.join(missing)}")
    if unknown:
        raise RuntimeError(f"training data has unknown labels: {', '.join(unknown)}")

    seen = set()
    duplicates = []
    for row in train_rows:
        key = normalize_text(row.get("text", ""))
        if not key:
            raise RuntimeError("training row has empty text")
        if key in seen:
            duplicates.append(row["text"])
        seen.add(key)
    if duplicates:
        raise RuntimeError(f"duplicate training utterances: {duplicates[:5]}")
    return counts


def mode_for_label(label):
    if label == ABSTAIN_LABEL:
        return MODE_ABSTAIN
    if label == "knowledge":
        return MODE_KNOWLEDGE
    return MODE_TOOL


def train_model(args, labels, train_rows, eval_rows):
    filtered_rows, leakage = remove_exact_eval_leakage(train_rows, eval_rows)
    train_counts = validate_training_data(filtered_rows, labels)
    label_to_id = {label: index for index, label in enumerate(labels)}

    dataset = Dataset.from_dict({
        "text": [row["text"] for row in filtered_rows],
        "label": [label_to_id[row["label"]] for row in filtered_rows],
    })

    device = choose_training_device(args.train_device)
    started = time.perf_counter()
    model = SetFitModel.from_pretrained(args.base_model, labels=labels, device=device)
    model_load_ms = (time.perf_counter() - started) * 1000

    training_args = TrainingArguments(
        output_dir=str(Path(args.model_dir) / "checkpoints"),
        batch_size=args.batch_size,
        num_epochs=args.epochs,
        seed=args.seed,
        save_strategy="no",
        logging_strategy="no",
        report_to="none",
    )
    trainer = Trainer(model=model, args=training_args, train_dataset=dataset)

    training_started = time.perf_counter()
    trainer.train()
    setfit_training_ms = (time.perf_counter() - training_started) * 1000

    # One encoder pass at runtime, two tiny linear heads in parallel:
    # 1) knowledge/tool/abstain gate, 2) tool capability if gate says tool.
    head_started = time.perf_counter()
    texts = [row["text"] for row in filtered_rows]
    embeddings = np.asarray(model.encode(texts, batch_size=64, show_progress_bar=False), dtype=np.float32)

    mode_targets = [mode_for_label(row["label"]) for row in filtered_rows]
    mode_head = LogisticRegression(
        max_iter=4000,
        class_weight="balanced",
        random_state=args.seed,
    )
    mode_head.fit(embeddings, mode_targets)

    tool_indices = [
        index for index, row in enumerate(filtered_rows)
        if row["label"] not in {"knowledge", ABSTAIN_LABEL}
    ]
    tool_embeddings = embeddings[tool_indices]
    tool_targets = [filtered_rows[index]["label"] for index in tool_indices]
    tool_head = LogisticRegression(
        max_iter=4000,
        class_weight="balanced",
        random_state=args.seed,
    )
    tool_head.fit(tool_embeddings, tool_targets)
    head_training_ms = (time.perf_counter() - head_started) * 1000

    model_dir = Path(args.model_dir)
    model.save_pretrained(model_dir)
    joblib.dump(mode_head, model_dir / MODE_HEAD_FILE)
    joblib.dump(tool_head, model_dir / TOOL_HEAD_FILE)

    mode_counts = Counter(mode_targets)
    print(
        f"trained rows={len(filtered_rows)} setfit_classes={len(labels)} device={device} "
        f"base={args.base_model} load_ms={model_load_ms:.1f} "
        f"setfit_train_ms={setfit_training_ms:.1f} head_train_ms={head_training_ms:.1f} "
        f"removed_exact_eval_leaks={len(leakage)} counts={dict(train_counts)} mode_counts={dict(mode_counts)}",
        file=sys.stderr,
    )
    if leakage:
        print(
            "excluded exact eval leakage: " + "; ".join(row["text"] for row in leakage[:5]),
            file=sys.stderr,
        )


def load_heads(model_dir):
    model_dir = Path(model_dir)
    return (
        joblib.load(model_dir / MODE_HEAD_FILE),
        joblib.load(model_dir / TOOL_HEAD_FILE),
    )


def benchmark_model(args, registry, eval_rows):
    load_started = time.perf_counter()
    model = SetFitModel.from_pretrained(args.model_dir, device="cpu")
    mode_head, tool_head = load_heads(args.model_dir)
    load_ms = (time.perf_counter() - load_started) * 1000

    warm_started = time.perf_counter()
    warm_embedding = np.asarray(
        model.encode(["warm up the capability classifier"], show_progress_bar=False),
        dtype=np.float32,
    )
    mode_head.predict_proba(warm_embedding)
    tool_head.predict_proba(warm_embedding)
    warmup_ms = (time.perf_counter() - warm_started) * 1000

    mode_class_to_index = {label: index for index, label in enumerate(mode_head.classes_)}
    output_rows = []

    for row in eval_rows:
        started = time.perf_counter()
        embedding = np.asarray(model.encode([row["text"]], show_progress_bar=False), dtype=np.float32)
        mode_probs = mode_head.predict_proba(embedding)[0]
        tool_probs = tool_head.predict_proba(embedding)[0]
        latency_ms = (time.perf_counter() - started) * 1000

        mode_index = int(np.argmax(mode_probs))
        route_mode = str(mode_head.classes_[mode_index])
        route_confidence = float(mode_probs[mode_index])

        tool_index = int(np.argmax(tool_probs))
        tool_primary = str(tool_head.classes_[tool_index])
        tool_confidence = float(tool_probs[tool_index])

        scores = {"knowledge": float(mode_probs[mode_class_to_index[MODE_KNOWLEDGE]])}
        tool_mass = float(mode_probs[mode_class_to_index[MODE_TOOL]])
        for label, probability in zip(tool_head.classes_, tool_probs):
            scores[str(label)] = tool_mass * float(probability)

        ranked = sorted(scores.items(), key=lambda item: item[1], reverse=True)
        score_margin = float(ranked[0][1] - ranked[1][1]) if len(ranked) > 1 else float(ranked[0][1])

        if route_mode == MODE_ABSTAIN:
            primary = None
            capabilities = []
            abstain = True
            need = "unknown"
        elif route_mode == MODE_KNOWLEDGE:
            primary = "knowledge"
            capabilities = ["knowledge"]
            abstain = False
            need = "stable"
        else:
            primary = tool_primary
            capabilities = [tool_primary]
            abstain = False
            need = source_need(tool_primary, registry)

        output_rows.append({
            "id": row["id"],
            "primary": primary,
            "capabilities": capabilities,
            "scores": scores,
            "source_need": need,
            "abstain": abstain,
            "route_mode": route_mode,
            "route_confidence": route_confidence,
            "tool_candidate": tool_primary,
            "tool_confidence": tool_confidence,
            "margin": score_margin,
            "latency_ms": latency_ms,
        })

    output_path = Path(args.output)
    with output_path.open("w", encoding="utf-8", newline="\n") as fh:
        for row in output_rows:
            fh.write(json.dumps(row, separators=(",", ":")) + "\n")

    print(
        f"benchmarked rows={len(eval_rows)} device=cpu load_ms={load_ms:.1f} warmup_ms={warmup_ms:.1f} "
        f"output={output_path}",
        file=sys.stderr,
    )


def main():
    parser = argparse.ArgumentParser(description="Train and benchmark one-pass hierarchical SetFit router v3")
    parser.add_argument("corpus", nargs="?", default="corpus.jsonl")
    parser.add_argument("output", nargs="?", default="setfit-v3-predictions.jsonl")
    parser.add_argument("--train", default="setfit-train.jsonl")
    parser.add_argument("--extra-train", default="setfit-train-v2-extra.jsonl")
    parser.add_argument("--capabilities", default="capabilities.json")
    parser.add_argument("--model-dir", default="setfit-minilm-router-v3")
    parser.add_argument("--base-model", default=DEFAULT_MODEL)
    parser.add_argument("--train-device", choices=["auto", "cpu", "cuda"], default="auto")
    parser.add_argument("--epochs", type=int, default=2)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--skip-train", action="store_true")
    args = parser.parse_args()

    eval_rows = read_jsonl(Path(args.corpus))
    train_rows = read_jsonl(Path(args.train))
    extra_path = Path(args.extra_train)
    if extra_path.exists():
        train_rows.extend(read_jsonl(extra_path))

    capabilities_doc = json.loads(Path(args.capabilities).read_text(encoding="utf-8"))
    capability_labels = [row["id"] for row in capabilities_doc["capabilities"]]
    labels = capability_labels + [ABSTAIN_LABEL]
    registry = {row["id"]: row for row in capabilities_doc["capabilities"]}

    if args.train_device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("--train-device cuda requested but torch.cuda.is_available() is false")

    if not args.skip_train:
        train_model(args, labels, train_rows, eval_rows)
    elif not Path(args.model_dir).exists():
        raise RuntimeError(f"--skip-train requested but model directory does not exist: {args.model_dir}")

    benchmark_model(args, registry, eval_rows)


if __name__ == "__main__":
    main()
