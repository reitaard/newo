# Newo Universal Capability Router Research

This directory is intentionally isolated from production routing. The goal is to choose a routing architecture from measurements before changing `assistant-routing.js` or tool exposure.

## Problem

Newo now has a working LFM web-tool loop, but giving the model every live tool on every turn creates false tool use and unnecessary latency. The router must decide what *kind of capability/data* a turn may need without duplicating the LFM's answer generation or reasoning.

The router is not an answer model. It may produce routing evidence such as capability probabilities, live-vs-stable need, composition, and confidence. Deterministic host policy remains responsible for tool exposure, safety, required-slot checks, and destructive authorization.

## Architecture under test

```text
final transcript
     |
     +--> structural parsers -------------------+
     |                                          |
     +--> candidate semantic classifier --------+--> deterministic policy
                                                |
                                                +--> direct structured source
                                                +--> LFM with no tools
                                                +--> LFM with a small tool set
                                                +--> clarify / abstain
```

`FAST/THINK` remains orthogonal. Capability routing asks *what resources are needed?* Reasoning routing asks *how much reasoning is needed?*

## Candidate routers

We will compare the same corpus against:

1. `structural-baseline` - deterministic parsers/rules only. No semantic model.
2. `prototype-embedding` - small sentence encoder, nearest prototypes/centroids.
3. `setfit-classifier` - sentence encoder plus a lightweight classifier head.
4. `small-encoder-classifier` - ModernBERT/mmBERT-class encoder. If justified, test a shared multi-head variant for capability, source need, and complexity.

Do not add a generative LLM router to the first benchmark. A front-door generative model would add latency and overlap with the main LFM.

## Phase-1 capability vocabulary

See `capabilities.json`. The vocabulary describes semantic capabilities, not provider/API names. Providers can change independently.

The initial source priority is:

1. local deterministic computation
2. specialized structured/live source
3. Newo internal source
4. open-world web
5. LFM internal knowledge for stable/general reasoning

This is a preference, not a hard pipeline. Policy can bypass LFM for exact safe direct results or feed structured results into one LFM generation when wording/reasoning is useful.

## Corpus design

`corpus.jsonl` deliberately includes:

- obvious positives
- paraphrases with no capability keyword
- hard negatives that contain misleading words
- stable-vs-live contrasts
- multi-capability requests
- requests with missing slots
- ambiguous cases where the router should abstain rather than force a tool

Do not train and evaluate on the same examples. The current file is a seed corpus/specification. Before model training, split by semantic cluster/paraphrase family rather than random rows so near-duplicates cannot leak across train/test.

## Output contract for router experiments

Each experimental router should emit one JSON object per corpus row:

```json
{
  "id": "w001",
  "scores": {
    "weather.forecast": 0.94,
    "knowledge": 0.03,
    "web.search": 0.02
  },
  "source_need": "live",
  "composition": "single",
  "abstain": false,
  "latency_ms": 12.4
}
```

Only `id` is mandatory for transport; benchmark adapters can normalize model-specific outputs before scoring.

## Metrics

Primary quality metrics:

- primary capability accuracy
- capability-set exact match
- top-2 capability recall
- false-tool rate on no-tool/stable requests
- false-web rate
- missed-live rate
- abstention precision/recall on ambiguous examples
- structured-source preference when a specialized capability exists

Performance metrics:

- p50 / p95 / p99 routing latency
- cold-start latency
- process RSS / model memory
- CPU utilization where available

Initial Newo latency target:

- p50 < 25 ms
- p95 < 50 ms

The target is a design goal, not a pass/fail requirement until measured on the VPS hardware.

## Policy boundary

The classifier must never become authorization. A future production policy should combine model signals with:

- parser-extracted slots
- tool availability
- confidence and top-class margin
- read-only / destructive metadata
- network/live requirement
- composition/dependency information
- host authorization for consequential tools

High-confidence read-only structured requests may route directly. Low-confidence cases should expose a small candidate set to LFM or abstain/clarify rather than forcing a route.

## Execution order

1. Lock capability vocabulary and seed corpus.
2. Add a deterministic structural baseline and score it.
3. Benchmark prototype embeddings.
4. Benchmark SetFit-style classifier.
5. Benchmark a small encoder classifier; only then consider a multi-head model.
6. Compare quality/latency/confusion matrices.
7. Select the smallest architecture meeting accuracy and false-tool/false-web targets.
8. Integrate behind a feature flag.
9. Re-run end-to-end Newo latency benchmark.

Production web routing should not be rewritten from this branch until the comparison is complete.
