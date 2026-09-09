# Phase 4 recording guide

Record only with informed consent. Treat CSI, person labels, activity labels,
notes, and any future camera linkage as privacy-sensitive research data. Keep
the generated `datasets/` directory outside Git, restrict access, and define a
retention/deletion date before human sessions.

## Fixed setup

1. Mark and photograph or diagram the router, Newo, and Newo2 positions without
   including identifying household details.
2. Configure the collector address and explicit AP/Newo/Newo2 station MACs.
3. Keep all three radios, furniture, doors, and movable objects fixed across a
   comparison block. Do not channel-hop.
4. Start with an `EMPTY` session, then record one scenario per session. Do not
   change labels during a capture.
5. Use a pseudonymous `--person` value, not a legal name. Use a pseudonymous
   `--room-id`, not an address.
6. Record duration, repetition number, route endpoints, and deviations in
   `--notes`. Prefer multiple independent repetitions over one long take.
7. Inspect the result immediately for all three paths, stable channels and CSI
   lengths, plausible rates, sequence gaps, and device drop counters.

Example baseline and labeled captures, run from `tools/`:

```sh
python -m newo_csi collect --room-id ROOM_A --scenario EMPTY --duration 120 \
  --router-bssid aa:bb:cc:dd:ee:ff --newo-mac 10:11:12:13:14:15 \
  --newo2-mac 20:21:22:23:24:25 \
  --notes "baseline 01; door closed; fixed topology"

python -m newo_csi collect --room-id ROOM_A --scenario WALK_DOOR_CENTER \
  --person seven --zone CENTER --activity WALKING --duration 30 \
  --router-bssid aa:bb:cc:dd:ee:ff --newo-mac 10:11:12:13:14:15 \
  --newo2-mac 20:21:22:23:24:25 \
  --notes "repeat 01; start at door marker; stop at center marker"
```

## Controlled scenario labels

| Scenario | Recording instruction |
| --- | --- |
| `EMPTY` | No person in the room; establish before/after baselines. |
| `ENTER` | Begin outside, enter through the marked doorway, stop at the marked endpoint. |
| `EXIT` | Begin at the marked indoor point and leave through the doorway. |
| `WALK_DOOR_CENTER` | Walk from the door marker to the center marker. |
| `WALK_CENTER_DOOR` | Walk from the center marker to the door marker. |
| `WALK_LEFT_RIGHT` | Traverse between fixed left and right markers. |
| `WALK_RIGHT_LEFT` | Reverse the left/right traversal. |
| `STAND` | Remain standing on one marked zone. |
| `SIT` | Sit in the marked seat with a recorded initial pose. |
| `LIE` | Lie in the consented, marked test location; this is an activity label, not fall detection. |
| `WAVE` | Stand at a fixed marker and perform a predefined arm wave. |
| `TURN` | Stand at a fixed marker and perform one predefined turn. |
| `FAST_WALK` | Follow one marked route at a repeatable brisk pace; do not run. |
| `SLOW_WALK` | Follow the same route at a repeatable slow pace. |

Labels are experimental ground truth, never model output. Randomize or balance
scenario order where practical, and retain negative/failed repetitions rather
than silently deleting inconvenient measurements.
