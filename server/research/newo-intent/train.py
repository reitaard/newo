import argparse
import json
from pathlib import Path

import numpy as np
from datasets import Dataset
from setfit import SetFitModel, Trainer, TrainingArguments
from setfit.exporters.onnx import export_onnx
from sklearn.metrics import classification_report
from sklearn.model_selection import train_test_split


def load_rows(path: Path):
    with path.open(encoding="utf-8") as stream:
        return [json.loads(line) for line in stream if line.strip()]


def main():
    parser = argparse.ArgumentParser(description="Train Newo's broad intent router on a CUDA machine.")
    parser.add_argument("--data", type=Path, default=Path(__file__).with_name("training.jsonl"))
    parser.add_argument("--output", type=Path, default=Path("artifacts/newo-intent-setfit"))
    parser.add_argument("--base-model", default="sentence-transformers/all-MiniLM-L6-v2")
    parser.add_argument("--epochs", type=int, default=4)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    rows = load_rows(args.data)
    labels = sorted({row["label"] for row in rows})
    train_rows, test_rows = train_test_split(rows, test_size=0.25, random_state=args.seed,
                                              stratify=[row["label"] for row in rows])
    train = Dataset.from_list(train_rows)
    test = Dataset.from_list(test_rows)
    model = SetFitModel.from_pretrained(args.base_model, labels=labels)
    trainer = Trainer(model=model, train_dataset=train, eval_dataset=test,
                      args=TrainingArguments(num_epochs=args.epochs, batch_size=16,
                                             num_iterations=20, seed=args.seed))
    trainer.train()
    predictions = model.predict(test["text"])
    print(classification_report(test["label"], predictions, zero_division=0))

    args.output.mkdir(parents=True, exist_ok=True)
    model.save_pretrained(args.output)
    export_onnx(model.model_body, model.model_head, opset=17, output_path=args.output / "model.onnx")
    metadata = {"labels": labels, "base_model": args.base_model, "train_rows": len(train),
                "test_rows": len(test), "seed": args.seed}
    (args.output / "training-metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(f"Saved Hugging Face SetFit and ONNX artifacts to {args.output}")


if __name__ == "__main__":
    main()
