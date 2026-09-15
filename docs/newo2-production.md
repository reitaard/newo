# Newo2 production boundary

Newo2 production firmware lives in top-level `Newo2/`. The old `experiments/goouuu-vision-bench/`, all `goouuu-*` branches, and `experiments/wifi-sensing-v1/` are retained as evidence/research and must not be deleted when production firmware advances.

## Layer contract

```text
trigger sources
  motion / future CSI / Telegram / Newo / automation
        |
        v
Newo2Events queue
        |
        v
router
        |
        +--> state controls (camera, motion)
        |
        +--> skills/motion_snapshot
                 |
                 +--> Newo2Camera one-shot JPEG
                 +--> Newo2Storage best-effort SD
                 +--> Newo2Network direct VPS upload
```

Triggers never own camera actions. Skills do not implement trigger detection. Camera code does not know Telegram, CSI, or AI.

Newo2 talks directly to the VPS for image transfer. Newo remains the voice/display/speaker unit and must not relay JPEGs. The VPS is the integration point between Newo, Newo2, Telegram and later multimodal AI.

## Deliberately deferred

- face recognition/enrollment
- person/face semantic gating
- video/live stream
- vision model inference
- CSI trigger adapter
- Telegram command adapter in the mature Newo process
- UXGA still profile
- hard camera power-down/deinit

Each can be added behind the existing event/skill boundaries after motion-snapshot v1 passes physical acceptance.
