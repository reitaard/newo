# Newo memory extraction benchmark

This branch benchmarks memory extraction only. It does not change the production assistant, ESP firmware, storage, retrieval, or prompts used by normal Newo requests.

## Synthetic extraction benchmark

- `minimal`: intentionally small Newo extraction contract.
- `voicemem`: VoiceMem-inspired additive extraction/filtering rules.
- Both use the same model, dataset, structured Ollama response schema, and sampling settings.
- FAST/no-think is the current baseline. THINK performed worse on the 24-case gold set.

Run:

```bash
cd /opt/newo/server
BENCH_MODES=FAST npm run memory:benchmark
```

Default target:

```text
OLLAMA_URL=http://100.110.136.15:11435/api/generate
OLLAMA_MODEL=newo-main
```

Useful controls:

```bash
BENCH_LIMIT=5 BENCH_MODES=FAST npm run memory:benchmark
BENCH_SEED=42 npm run memory:benchmark
BENCH_MAX_TOKENS=256 npm run memory:benchmark
BENCH_DATASET=/path/to/dataset.json npm run memory:benchmark
BENCH_STRUCTURED=0 npm run memory:benchmark
```

## LoCoMo Oracle-Gap diagnostic

`npm run memory:locomo` is a model-vs-memory-compression diagnostic, not a full retrieval benchmark.

It downloads and caches the official `snap-research/locomo` `data/locomo10.json` file pinned to commit `3eb6f2c585f5e1699204e3c3bdf7adc5c28cb376`. LoCoMo QA annotations provide the gold dialog IDs in `evidence`; the benchmark intentionally uses those gold turns so retrieval quality is removed from the experiment.

Default selection is 30 QA items:

- 10 category `4` single-hop
- 10 category `1` multi-hop
- 10 category `2` temporal

The selector first tries to take one eligible item per LoCoMo conversation for each category, then fills any remaining quota deterministically.

Each question runs three lanes with the same model:

```text
Gold LoCoMo evidence
        |
        +--> oracle: answer directly from gold turns
        |
        +--> minimal extractor --> compressed memories --> answer
        |
        +--> VoiceMem-style extractor --> compressed memories --> answer
```

The important number is `compression_gap_vs_oracle`:

```text
oracle F1 - memory-lane F1
```

A low oracle score points toward a model capability problem. A high oracle score plus a large compression gap points toward the extraction/compression layer. A later full-memory LoCoMo run can then isolate retrieval/ranking loss.

Smoke test first:

```bash
LOCOMO_PER_CATEGORY=1 npm run memory:locomo
```

Full 30-question diagnostic:

```bash
npm run memory:locomo
```

Run only the raw oracle lane to compare model capability cheaply:

```bash
LOCOMO_LANES=oracle npm run memory:locomo
```

Compare another model available on the same Ollama host:

```bash
LOCOMO_MODEL=qwen3:8b LOCOMO_LANES=oracle npm run memory:locomo
```

Useful controls:

```bash
LOCOMO_PER_CATEGORY=5 npm run memory:locomo
LOCOMO_CATEGORIES=4,1,2 npm run memory:locomo
LOCOMO_LANES=oracle,minimal,voicemem npm run memory:locomo
LOCOMO_MODEL=newo-main npm run memory:locomo
LOCOMO_OLLAMA_BASE_URL=http://100.110.136.15:11435 npm run memory:locomo
LOCOMO_DATASET=/path/to/locomo10.json npm run memory:locomo
LOCOMO_REFRESH=1 npm run memory:locomo
```

The script reports overall and per-category F1, answer latency, extractor latency, mean memory count, and the compression gap. Detailed JSON is written under `benchmarks/memory/results/`.

Scoring intentionally uses a small dependency-free JS implementation of LoCoMo-style lexical F1 normalization without Porter stemming. Use it for within-run lane/model comparisons; do not present it as an official LoCoMo leaderboard score.

## Decision gate

Do not wire either extractor into production memory from this branch. Use the synthetic gold set to debug extraction behavior, then use Oracle-Gap to decide whether the bottleneck is the model or compression. Only after those pass should we run a full long-term storage/retrieval LoCoMo evaluation and choose what moves into production.
