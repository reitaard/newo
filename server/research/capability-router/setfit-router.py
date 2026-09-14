import argparse
import json
import os
import sys
import time
from collections import Counter
from pathlib import Path

import numpy as np
import torch
from datasets import Dataset
from setfit import SetFitModel, Trainer, TrainingArguments

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


def normalize_text(text):
    return " ".join(str(text).lower().split())


def source_need(capability, registry):
    meta = registry.get(capability, {})
    family = str(meta.get("family", ""))
    if family.startswith("internal") or meta.get("source") in {"memory", "device", "sensor", "camera"}:
        return "internal"
    return "live" if meta.get("live") else "stable"


def to_numpy(value):
    if hasattr(value, "detach"):
        value = value.detach().cpu().numpy()
    return np.asarray(value)


def choose_training_device(requested):
    if requested != "auto":
        return requested
    return "cuda" if torch.cuda.is_available() else "cpu"


def validate_training_data(train_rows, eval_rows, labels):
    train_labels = Counter(row.get("label") for row in train_rows)
    missing = [label for label in labels if not train_labels.get(label)]
    unknown = sorted(set(train_labels) - set(labels))
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

    eval_texts = {normalize_text(row.get("text", "")) for row in eval_rows}
    overlap = [row["text"] for row in train_rows if normalize_text(row["text"]) in eval_texts]
    if overlap:
        raise RuntimeError(f"exact train/eval leakage detected: {overlap[:5]}")

    return train_labels


def train_model(args, labels, train_rows, eval_rows):
    train_counts = validate_training_data(train_rows, eval_rows, labels)
    label_to_id = {label: index for index, label in enumerate(labels)}
    dataset = Dataset.from_dict({
        "text": [row["text"] for row in train_rows],
        "label": [label_to_id[row["label"]] for row in train_rows],
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
    training_ms = (time.perf_counter() - training_started) * 1000

    model.save_pretrained(args.model_dir)
    print(
        f"trained rows={len(train_rows)} classes={len(labels)} device={device} "
        f"base={args.base_model} load_ms={model_load_ms:.1f} train_ms={training_ms:.1f} "
        f"counts={dict(train_counts)}",
        file=sys.stderr,
    )


def benchmark_model(args, labels, registry, eval_rows):
    load_started = time.perf_counter()
    model = SetFitModel.from_pretrained(args.model_dir, device="cpu")
    load_ms = (time.perf_counter() - load_started) * 1000

    if list(model.labels or []) != labels:
        raise RuntimeError(f"saved model label order mismatch: {model.labels}")

    warm_started = time.perf_counter()
    model.predict_proba(["warm up the capability classifier"])
    warmup_ms = (time.perf_counter() - warm_started) * 1000

    output_rows = []
    for row in eval_rows:
        started = time.perf_counter()
        probs = to_numpy(model.predict_proba([row["text"]]))
        latency_ms = (time.perf_counter() - started) * 1000
        if probs.ndim == 1:
            vector = probs
        elif probs.ndim == 2 and probs.shape[0] == 1:
            vector = probs[0]
        else:
            raise RuntimeError(f"unexpected probability shape {probs.shape}")
        if len(vector) != len(labels):
            raise RuntimeError(f"probability width {len(vector)} != label count {len(labels)}")

        best = int(np.argmax(vector))
        primary = labels[best]
        scores = {label: float(vector[index]) for index, label in enumerate(labels)}
        output_rows.append({
            "id": row["id"],
            "primary": primary,
            "capabilities": [primary],
            "scores": scores,
            "source_need": source_need(primary, registry),
            "abstain": False,
            "latency_ms": latency_ms,
        })

    output_path = Path(args.output)
    with output_path.open("w", encoding="utf-8", newline="\n") as fh:
        for row in output_rows:
            fh.write(json.dumps(row, separators=(",", ",")) + "\n")

    print(
        f"benchmarked rows={len(eval_rows)} device=cpu load_ms={load_ms:.1f} warmup_ms={warmup_ms:.1f} "
        f"output={output_path}",
        file=sys.stderr,
    )


def main():
    parser = argparse.ArgumentParser(description="Train and benchmark a SetFit capability router")
    parser.add_argument("corpus", nargs="?", default="corpus.jsonl")
    parser.add_argument("output", nargs="?", default="setfit-predictions.jsonl")
    parser.add_argument("--train", default="setfit-train.jsonl")
    parser.add_argument("--capabilities", default="capabilities.json")
    parser.add_argument("--model-dir", default="setfit-minilm-router")
    parser.add_argument("--base-model", default=DEFAULT_MODEL)
    parser.add_argument("--train-device", choices=["auto", "cpu", "cuda"], default="auto")
    parser.add_argument("--epochs", type=int, default=2)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--skip-train", action="store_true")
    args = parser.parse_args()

    eval_rows = read_jsonl(Path(args.corpus))
    train_rows = read_jsonl(Path(args.train))
    capabilities_doc = json.loads(Path(args.capabilities).read_text(encoding="utf-8"))
    labels = [row["id"] for row in capabilities_doc["capabilities"]]
    registry = {row["id"]: row for row in capabilities_doc["capabilities"]}

    if args.train_device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("--train-device cuda requested but torch.cuda.is_available() is false")

    if not args.skip_train:
        train_model(args, labels, train_rows, eval_rows)
    elif not Path(args.model_dir).exists():
        raise RuntimeError(f"--skip-train requested but model directory does not exist: {args.model_dir}")

    benchmark_model(args, labels, registry, eval_rows)


if __name__ == "__main__":
    main()
