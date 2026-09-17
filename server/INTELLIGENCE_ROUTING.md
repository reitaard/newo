# Newo intelligence routing

## Current decision

Keep routing as a layered system:

1. High-confidence deterministic shortcuts for nearly deterministic intents and latency-sensitive commands.
2. Liquid semantic routing for natural-language requests that do not match a shortcut.
3. Existing capability/tool resolution and host-side authorization.
4. Separate complexity/execution policy for FAST/THINK and model choice.

Deterministic shortcuts are intentionally small. They must not grow into a hand-written replacement for semantic classification.

## Liquid zero-shot baseline

Model: `LiquidAI/LFM2.5-Encoder-350M-Prompt-Router`

V4 route descriptions, 96-prompt benchmark:

- Overall: 73/96 = 76.0%
- Boundary cases: 14/23 = 60.9%
- Typical warm CPU routing latency: about 0.7 s on the current VPS with 2 PyTorch threads

The main failure pattern is not random. The router often identifies the subject but misses the user's intent when both are present. Examples include camera vs troubleshooting, sensor vs troubleshooting, and device status vs device control.

This means the 12-lane flat taxonomy mixes two dimensions:

- domain/subject: camera, sensor, device, weather, finance, etc.
- intent: control, status, recall, troubleshooting, conversation, etc.

Do not assume every such failure is fixed by route wording alone.

## Deterministic shortcut benchmark

The conservative shortcut layer handled 38/96 prompts in the tuning corpus and all 38 matched their expected lane. Combined score on that same corpus was 90/96 = 93.8%, with boundary accuracy 20/23 = 87.0%.

Treat this only as an in-sample engineering result because the shortcuts were designed after inspecting failures in the same corpus. It is not a production generalization estimate.

The shortcut layer is valuable mainly because obvious requests can skip the roughly 0.7 s Liquid pass.

## Next semantic-model experiment

Before expanding regex coverage or replacing the 350M encoder, test task-specific supervised routing.

Preferred experiment:

- keep the same 350M LFM2.5 encoder family
- train on Newo-specific examples with explicit lane labels
- use separate train/dev/holdout splits
- keep the holdout untouched while tuning
- compare against the zero-shot Prompt Router baseline
- report overall accuracy, boundary accuracy, per-lane precision/recall, confusion matrix, and CPU latency

Natural-language cases such as `My ESP keeps dropping Wi-Fi every few minutes` should be solved by the semantic classifier, not by accumulating regex paraphrases.

If a supervised flat 12-lane classifier still struggles, test a two-axis representation such as `intent` plus `domain` rather than immediately increasing deterministic rules.

## Production constraint

Routing classification never authorizes a mutating or destructive action. Existing host authorization and fail-open safety behavior remain independent of the classifier.
