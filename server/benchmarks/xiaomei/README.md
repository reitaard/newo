# Xiaomei model benchmark

This benchmark is for the Xiaomei/Ling Ling controller model shootout. It is intentionally model-agnostic and keeps the context fixed at 4096 so MiniCPM5-2B, Qwen3.5-2B, Spark-X2.5-1.7B, and any later challenger are compared under the same conditions.

The suite focuses on the behavior Xiaomei actually needs:

- English and Chinese casual chat
- Chinese teaching/explanation
- mixed English/Chinese input
- one-shot translation intent
- entering/exiting continuous interpreter mode
- distinguishing speech to the other person from meta-questions to Xiaomei
- cold/warm latency and Ollama-reported residency

## Important benchmark rule

For official comparisons, change only the candidate model/endpoint in the existing `main` profile slot and commit that swap separately. Do not retune the fixture or context between candidates.

The benchmark reads the `main` assistant profile by default, but uses its own fixed Xiaomei system prompt and sampling settings. This avoids changing the production Alfred prompt while we compare candidate models.

## Run

From `/opt/newo/server`:

```bash
npm test
npm run check
npm run xiaomei:benchmark
```

Defaults:

- profile slot: `main`
- context: `4096`
- repeats: `1`
- thinking: off
- keep-alive: forever during the warm run

Optional diagnostics only:

```bash
XIAOMEI_BENCH_REPEATS=3 npm run xiaomei:benchmark
XIAOMEI_BENCH_BASE_URL=http://HOST:11435 XIAOMEI_BENCH_MODEL=model-id npm run xiaomei:benchmark
```

For the official shootout, prefer a committed profile swap over environment overrides so every report maps to an exact git commit.

## Reports

Each run writes both JSON and Markdown:

```text
server/benchmarks/xiaomei/results/<model>__<timestamp>.json
server/benchmarks/xiaomei/results/<model>__<timestamp>.md
server/benchmarks/xiaomei/results/LATEST.json
server/benchmarks/xiaomei/results/LATEST.md
```

The JSON keeps every raw model reply. The Markdown is the review report with route accuracy, JSON compliance, first-content latency, total latency, prefill/decode speed, cold-load measurements, and Ollama `/api/ps` residency data.

The report also leaves a human-review section for Chinese quality, code switching, teaching quality, interpreter judgment, and voice-response suitability.

## Candidate order

Current baseline and challengers:

1. MiniCPM5-2B — baseline
2. Qwen3.5-2B
3. Spark-X2.5-1.7B
4. Gemma 4 E2B — existing control result

Hy-MT2-1.8B is not part of this controller test. It gets a separate translation benchmark after the controller winner is narrowed down.
