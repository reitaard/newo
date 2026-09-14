# Capability Router Dataset Research

Branch purpose: replace the hand-built-only training corpus with a reproducible corpus grounded in established intent / voice-assistant / function-calling datasets, while keeping the Newo capability taxonomy and production architecture unchanged.

## Current finding

The live misroute `capital of France -> weather.current` is most consistent with a dataset-boundary problem, not a provider or integration bug. The current SetFit v2 training corpus is too small and hand-curated to establish robust boundaries such as geographic knowledge vs live weather.

Do not patch individual failures with regexes, keywords, or another front-end classifier. Fix the classifier data and re-evaluate on untouched data.

## Best public sources

### 1. MASSIVE-Agents — primary anchor

- Amazon Science / EMNLP 2025.
- 47,020 benchmark samples across 52 languages, 55 functions and 286 arguments.
- Derived by cleaning MASSIVE and converting it into BFCL-style function calls.
- License: CC BY 4.0.
- Paper: https://www.amazon.science/publications/massive-agents-a-benchmark-for-multilingual-function-calling-in-52-languages
- Dataset: https://huggingface.co/datasets/AmazonScience/massive-agents

Why it fits Newo: this is already a voice-assistant corpus expressed as function selection + arguments rather than only intent labels. It covers functions corresponding closely to maths, currency, stock, datetime, weather, IoT control, query/state and other assistant tasks.

Use the English subset first. Preserve source IDs so train/dev/test provenance stays auditable.

### 2. MASSIVE — voice-assistant intent + slot anchor

- Amazon Science / ACL 2023.
- >1M localized utterances, 52 languages, 60 intents, 55 slot types, 18 domains.
- English source is derived from SLURP and contains realistic assistant-style requests.
- License: CC BY 4.0.
- Dataset: https://huggingface.co/datasets/AmazonScience/massive
- Paper: https://www.amazon.science/publications/massive-a-1m-example-multilingual-natural-language-understanding-dataset-with-51-typologically-diverse-languages

High-value intents for Newo include `qa_maths`, `qa_currency`, `qa_stock`, `datetime_query`, `datetime_convert`, `weather_query`, `qa_factoid`, `qa_definition`, `news_query`, and multiple `iot_*` intents.

Important: do not map broad labels blindly. `weather_query`, for example, can contain both current and forecast utterances; use its slots / temporal wording to split only high-confidence examples into `weather.current` and `weather.forecast`, otherwise exclude them from supervised target-label training.

### 3. HWU64 — home / device boundary data

- 25,716 utterances, 64 intents, 21 domains.
- Designed around human-home robot interaction.
- License reported as CC BY-SA 3.0.
- Reference benchmark stats: https://aclanthology.org/2021.naacl-industry.38.pdf
- Dataset mirror / examples: https://huggingface.co/datasets/DeepPavlov/hwu64

Why it fits Newo: strong source for device query/action language, alarms, home automation and assistant phrasing. It should improve `device.state` vs `device.control` boundaries much more reliably than a few hand-written examples.

### 4. CLINC150 / OOS — knowledge and abstention boundary

- EMNLP 2019.
- 150 in-scope intents across 10 domains plus explicit out-of-scope data.
- Full set: 100 train, 20 validation and 30 test examples per in-scope intent; explicit OOS train/validation/test samples.
- License: CC BY 3.0.
- Repository: https://github.com/clinc/oos-eval
- Paper: https://aclanthology.org/D19-1131/

Why it fits Newo: use it primarily to strengthen `knowledge` / unsupported / abstention boundaries, not as a direct one-to-one capability map. It is especially useful because production routers must not assume every utterance belongs to a tool intent.

### 5. BFCL — evaluation source, not primary training source

- Berkeley Function Calling Leaderboard.
- Includes function relevance detection, chatting/no-tool cases, multiple functions, parallel calls, multi-turn, web-search and memory tasks.
- Current benchmark: https://gorilla.cs.berkeley.edu/leaderboard
- Data documentation: https://github.com/EnlightenedAI/BFCL/blob/main/berkeley-function-call-leaderboard/bfcl_eval/data/README.md

Use BFCL mostly as an external evaluation/checking source so we retain an independent test of tool relevance and no-tool behavior. Avoid consuming all relevant BFCL data into training and then using the same categories to claim generalization.

## Recommended target mapping strategy

Only import examples where the source annotation gives high-confidence evidence for a Newo capability. Keep source labels alongside target labels.

Initial high-confidence examples:

- `MASSIVE qa_maths` -> `calculator`
- `MASSIVE datetime_convert` -> `time.convert`
- `MASSIVE qa_stock` -> `market.quote` only where the utterance requests a current price / quote; company knowledge stays out
- `MASSIVE qa_currency` -> `currency.exchange` only for exchange/conversion requests
- `MASSIVE weather_query` -> current / forecast only after temporal-slot separation
- `MASSIVE qa_factoid`, `qa_definition` -> `knowledge`
- `MASSIVE news_query` -> `web.search`
- action-oriented `iot_*` -> `device.control`
- read/query-oriented device examples from HWU64 -> `device.state`

Classes with little public coverage (`memory.retrieve`, `sensor.read`, `camera.inspect`, `web.read`) still need Newo-specific human-reviewed data. Public data should supplement, not erase, those domain-specific examples.

## Critical hard-negative families

Training/evaluation must explicitly separate concepts that share entities but require different sources:

- geographic fact vs weather: `capital of France` vs `weather in France`
- sports history/knowledge vs live score/schedule
- explanation of exchange rates vs current exchange rate
- company / stock knowledge vs current quote
- historical event/news vs latest/current news
- device description vs current device state vs device mutation
- visual/general knowledge vs current camera inspection
- prior-conversation wording vs general knowledge about memory

Do not add only the exact failed sentence. Add diverse examples from each failure family and test on different entities and wording.

## Training experiment

Keep the architecture fixed initially: MiniLM + SetFit. The first question is whether better data fixes the boundary errors; do not confound that with another backbone.

Experiment A:
1. Build a provenance-preserving public corpus from high-confidence mappings above.
2. Add the existing Newo-specific examples only for capabilities not covered well by public data.
3. Deduplicate exact and near-paraphrase examples across splits.
4. Split before training by source utterance / paraphrase cluster, not random rows.
5. Train the same SetFit architecture.
6. Evaluate on a fresh blind Newo set that is never used for tuning.

A useful optional experiment B is representation pretraining across public intent datasets before Newo fine-tuning. Prior work shows that contrastive training across intent corpora can improve discrimination among semantically similar intents, but this should be tested rather than assumed:
- https://aclanthology.org/2021.emnlp-main.144/

SetFit itself is already contrastive + classifier training, so begin with the simpler mapped-public-data experiment before adding another pretraining stage.

## Evaluation protocol

The existing 100-row corpus is a development set because earlier v2 changes were informed by its failures. Do not use it as the final blind score.

Create a new frozen blind set before the new model is trained. Include natural spoken phrasing, ASR-like awkwardness and different entities from the training examples.

Report at minimum:
- primary accuracy
- exact capability-set match
- top-2 primary recall
- false-tool rate
- false-web rate
- missed-live rate
- abstention precision/recall
- specialized-to-web error rate
- per-class precision/recall/F1
- confusion matrix
- confidence / margin distributions
- CPU p50 / p95 latency

Also report boundary-specific slices for geography/weather, sports knowledge/live, FX explanation/live, company/quote, device state/control, and OOS/abstain.

## Dataset quality rules

- Never train on the blind Newo set.
- Never patch individual blind failures and rerun the same set as if still blind.
- Keep public dataset IDs and source licenses in generated rows.
- Keep train/dev/test source provenance.
- Do not label-map ambiguous source intents automatically; exclude uncertain rows or route them to human review.
- Prefer real human/crowd utterances over generated paraphrases.
- Synthetic data may fill genuine Newo-only gaps, but it must be separately tagged and never dominate a class.
- Keep class balance controlled; do not let MASSIVE-heavy categories swamp Newo-specific classes.

## Decision from research

The strongest starting point is **MASSIVE-Agents + MASSIVE + HWU64 + CLINC150/OOS**, with BFCL held mostly for external evaluation. MASSIVE-Agents is especially valuable because it is both recent and already function-call oriented, which is closer to Newo's capability-routing task than our original hand-written corpus.

Do not retrain production from this branch yet. First build the reproducible importer/mapping and a fresh blind benchmark, audit the resulting class distributions and boundary coverage, then train a candidate artifact and compare it against frozen v2.
