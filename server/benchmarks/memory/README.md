# Newo memory extraction benchmark

This branch benchmarks memory extraction only. It does not change the production assistant, ESP firmware, storage, retrieval, or prompts used by normal Newo requests.

## Variants

- `minimal`: intentionally small Newo extraction contract.
- `voicemem`: VoiceMem-inspired additive extraction rules, including aggressive filtering of one-off requests, assistant output, voice/connection chatter, identity probes, uncertain statements, and other retrieval-polluting junk.

Both variants use the same dataset, model, sampling settings, and Ollama transport so quality/latency are directly comparable. This is a prompt/architecture comparison using Newo's model; it is not the full VoiceMem package runtime.

## Run from the VPS

```bash
cd ~/newo/server
npm run memory:benchmark:minimal
npm run memory:benchmark:voicemem
npm run memory:benchmark
```

Default target:

```text
OLLAMA_URL=http://100.68.131.86:11435/api/generate
OLLAMA_MODEL=newo-main
```

Override either when the Newo host changes. Useful controls:

```bash
BENCH_MODES=FAST npm run memory:benchmark
BENCH_MODES=THINK npm run memory:benchmark
BENCH_LIMIT=5 npm run memory:benchmark
BENCH_SEED=42 npm run memory:benchmark
BENCH_MAX_TOKENS=256 npm run memory:benchmark
BENCH_DATASET=/path/to/dataset.json npm run memory:benchmark
```

Each run writes detailed JSON under `benchmarks/memory/results/` and reports JSON validity, remember/no-memory decision accuracy, fact precision/recall/F1, false-memory count, type accuracy, temporal accuracy, TTFT, total latency, token counts, and generation throughput.

## Decision gate

Do not wire either extractor into production memory from this branch. First compare Minimal vs VoiceMem-style on the gold set. Then add a LoCoMo adapter that emits the same benchmark case schema and repeat the comparison on realistic multi-session evidence. A full VoiceMem runtime adapter can also be added as a third independent competitor later. Only the best validated approach should move into production.
