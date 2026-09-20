# Wi-Fi sensing recording guide

Record human sessions only with informed consent. Treat CSI, person/activity labels, notes, and any future camera linkage as privacy-sensitive research data. Keep generated `datasets/` outside Git and define retention/deletion rules before long captures.

## Fixed setup

1. Keep the router, Newo, and Newo2 in fixed positions during a comparison block.
2. Keep both ESPs on the same associated 2.4 GHz AP channel; do not channel-hop.
3. Configure explicit router/Newo/Newo2 identities in the collector when doing reproducible captures.
4. Keep furniture and large movable objects fixed unless the experiment is specifically about them.
5. Record the real room condition honestly. Do not label an occupied room `EMPTY`.
6. Prefer multiple independent repetitions over one long mixed take for gesture learning.
7. Inspect every capture for expected paths, stable channels, CSI geometry, sequence gaps, ring drops, transport drops, association epochs, and ESP-NOW diagnostics.

A perfectly empty room is **not required** if the real deployment room cannot provide one. Use a repeatable background condition instead, for example `LIE` with notes such as “one person lying mostly still; idle phones/laptops present”. The important rule is consistent geometry and truthful labels.

## Canonical capture

From `tools/`:

```sh
python -m newo_csi collect \
  --room-id ROOM_A \
  --scenario LIE \
  --duration 120 \
  --person "one person" \
  --notes "static occupied baseline; idle room devices present; fixed topology" \
  --router-bssid aa:bb:cc:dd:ee:ff \
  --newo-mac 10:11:12:13:14:15 \
  --newo2-mac 20:21:22:23:24:25
```

The collector stores exact NCSI datagrams in `frames.ncsi` plus `session.json`, `events.jsonl`, and `summary.json`. `summary.json` is useful for capture health; movement/gesture analysis must use the raw time series rather than only packet counts or mean RSSI.

## Scenario labels

| Scenario | Recording instruction |
| --- | --- |
| `EMPTY` | Truly no person in the room. Use only when this is actually achievable. |
| `ENTER` | Begin outside, enter through the marked doorway, stop at the marked endpoint. |
| `EXIT` | Begin at the marked indoor point and leave through the doorway. |
| `WALK_DOOR_CENTER` | Walk from door marker to center marker. |
| `WALK_CENTER_DOOR` | Reverse that route. |
| `WALK_LEFT_RIGHT` | Traverse between fixed left/right markers. |
| `WALK_RIGHT_LEFT` | Reverse the left/right traversal. |
| `STAND` | Remain standing at a fixed marker. |
| `SIT` | Sit in a marked seat with a recorded initial pose. |
| `LIE` | Lie/rest in a marked location. This is an activity label, not fall or sleep-stage detection. |
| `WAVE` | Perform one predefined arm-wave gesture at a fixed marker. |
| `TURN` | Perform one predefined turn at a fixed marker. |
| `FAST_WALK` | Repeat a brisk marked route. |
| `SLOW_WALK` | Repeat the same route slowly. |

Use `--activity` and `--notes` to describe conditions not represented by the fixed scenario enum, such as “two people walking/talking freely” or “overnight resting background”.

## Gesture-development protocol

The practical target is a Newo gesture trigger. Do not begin with many gesture classes.

Start with three broad states:

1. **still/background**
2. **walking/general motion**
3. **one deliberately repeated gesture**

For the first gesture, `WAVE` is the simplest existing label. Define the gesture before recording: same approximate body location, direction, size, and repetition count.

A useful first block is:

```text
background:  5 x 20–30 s
walking:     5 x 20–30 s
gesture:    20+ short independent repetitions
```

Keep some sessions completely held out from feature tuning. Ordinary busy-room captures and overnight resting captures should be retained as **negative/background** data so a gesture trigger can be tested against natural movement.

Packet rate is not a gesture feature: controlled gateway traffic itself changes packet cadence. Prefer geometry-aware I/Q features such as short-window amplitude variance, phase change, motion energy, and agreement across paths.

## Busy-room captures

For natural movement/background testing, `FAST_WALK` may be used as the nearest available scenario enum while the true condition is written in `--activity`/`--notes`.

Example:

```sh
python -m newo_csi collect \
  --room-id ROOM_A \
  --scenario FAST_WALK \
  --duration 300 \
  --activity "two people walking talking and moving freely around room" \
  --notes "natural busy-room background; router/Newo/Newo2 fixed" \
  --router-bssid aa:bb:cc:dd:ee:ff \
  --newo-mac 10:11:12:13:14:15 \
  --newo2-mac 20:21:22:23:24:25
```

Do not later relabel this as a clean controlled gesture session.

## Overnight background capture

Long overnight sessions are useful for false-trigger/background research, not for making sleep-stage claims.

For the first validated topology:

```text
Newo  -> fixed, USB-connected to collector PC for serial logging
Newo2 -> fixed separately, powered from a power bank
router -> fixed
people -> natural sleeping/resting
```

Start a normal collector capture **without `--duration`** and stop it with Ctrl+C in the morning. The collector finalizes `session.json` and `summary.json` on Ctrl+C.

Example:

```sh
python -m newo_csi collect \
  --room-id ROOM_A \
  --scenario LIE \
  --activity "overnight sleep background" \
  --person "two people" \
  --notes "two people sleeping/resting naturally; turns and small movements; fixed RF geometry; Newo2 on powerbank; ESP-NOW enabled" \
  --router-bssid aa:bb:cc:dd:ee:ff \
  --newo-mac 10:11:12:13:14:15 \
  --newo2-mac 20:21:22:23:24:25
```

With two people present, the RF signal is a mixture. Do not assume a detected motion belongs to a particular person.

## Serial logging

If Newo remains attached to the PC, a timestamped serial log can run in parallel with the UDP collector. Newo2 can remain on a power bank; its association, CSI, transport, and ESP-NOW counters still reach the host through NCSI STATUS/DIAGNOSTIC records.

Keep the PC awake for long captures and ensure adequate free disk space.

## Quality checks after every capture

Check:

- expected paths are present;
- channel is stable;
- receiver/source identities match the configured mapping;
- `ring_full_drops` stays at zero;
- rate-gate drops are understood for the active firmware variant;
- device UDP/sequence gaps are recorded rather than hidden;
- association epoch does not unexpectedly change;
- CSI length/PHY geometry distribution is recorded;
- for ESP-NOW, application probe counters are checked separately from peer CSI frame count.

A peer-path CSI frame is not necessarily one application ESP-NOW probe. Use `probe_tx_*` and `probe_rx_*` diagnostics for probe delivery.

## Labels are ground truth, not predictions

Scenario/person/activity labels describe what the operator intended or observed during collection. They are not model output. Preserve failed/noisy repetitions rather than deleting inconvenient data, and document deviations in `--notes`.
