# Xiaomei model benchmark

This benchmark is for the Xiaomei/Ling Ling controller model shootout. The official run is preconfigured for the three downloaded VPS candidates and keeps context fixed at 4096 so they are compared under the same conditions.

The suite focuses on the behavior Xiaomei actually needs:

- English and Chinese casual chat
- Chinese teaching/explanation
- mixed English/Chinese input
- one-shot translation intent
- entering/exiting continuous interpreter mode
- distinguishing speech to the other person from meta-questions to Xiaomei
- cold/warm latency and Ollama-reported residency

## Official candidates

The endpoint/model list lives in `benchmarks/xiaomei/candidates.json`:

1. MiniCPM5-2B — `newo-minicpm5:latest`
2. Qwen3.5-2B — `newo-qwen35-2b:latest`
3. Spark-X2.5-1.7B — `newo-spark17:latest`

All three currently use `http://100.110.136.15:11435/api/chat`.

`newo-spark-x2.5:latest` is recorded as an alias of the Spark 1.7B candidate because it currently resolves to the same digest. The archived 4B rollback model is intentionally not part of the first shootout.

Hy-MT2-1.8B is not part of this controller test. It gets a separate translation benchmark after the controller winner is narrowed down.

## One-command run

From `/opt/newo/server`:

```bash
npm run xiaomei:benchmark
```

That command runs all three controller candidates sequentially. Production Alfred profiles are not changed.

For a full branch validation first:

```bash
npm test
npm run check
npm run xiaomei:benchmark
```

Defaults for every candidate:

- context: `4096`
- repeats: `1`
- thinking: off
- temperature: `0.2`
- top_p: `0.9`
- keep-alive: forever during its warm run
- explicit unload after its report is captured, before the next candidate

To repeat each case three times:

```bash
XIAOMEI_BENCH_REPEATS=3 npm run xiaomei:benchmark
```

## Reports

Each candidate still writes its own full JSON and Markdown report with raw replies:

```text
server/benchmarks/xiaomei/results/<model>__<timestamp>.json
server/benchmarks/xiaomei/results/<model>__<timestamp>.md
```

The shootout also writes a combined comparison:

```text
server/benchmarks/xiaomei/results/SHOOTOUT-LATEST.json
server/benchmarks/xiaomei/results/SHOOTOUT-LATEST.md
server/benchmarks/xiaomei/results/SHOOTOUT__<timestamp>.json
server/benchmarks/xiaomei/results/SHOOTOUT__<timestamp>.md
```

The combined report compares route accuracy, JSON compliance, reply checks, first-content latency, total latency, prefill speed, decode speed, and Ollama-reported VRAM residency. The JSON embeds every candidate's complete per-case report so raw Chinese/English replies remain available for human review.

Do not select the winner from speed alone. Review Chinese fluency, code switching, teaching quality, interpreter/meta-command judgment, and voice-response brevity after the automated run.

## Single-model diagnostics

`candidate.json` remains deliberately isolated for ad-hoc diagnostics and does not modify production profiles. To use it, fill that file or pass environment overrides, then run:

```bash
npm run xiaomei:benchmark:single
```

Example:

```bash
XIAOMEI_BENCH_BASE_URL=http://100.110.136.15:11435 \
XIAOMEI_BENCH_MODEL=newo-minicpm5:latest \
npm run xiaomei:benchmark:single
```
