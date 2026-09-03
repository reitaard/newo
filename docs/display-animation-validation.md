# Newo display animation validation

This checklist validates the procedural eye engine without changing its timing or expression parameters merely to make a test easier to pass.

## 1. Host engine test

The pose and gaze engines deliberately avoid Arduino-only dependencies. On a machine with a C++17 compiler:

```sh
bash tools/run-display-animation-host-test.sh
```

Expected result:

```text
display-animation-host-test: PASS
```

The host test exercises the actual `NewoEyePoseEngine` and `NewoGazeMotion` C++ sources. It verifies:

- requesting the same pose target every display frame does not restart an active transition;
- filled-to-curved sleep closure switches late rather than at the midpoint;
- curved-to-filled wake closure switches early enough to open smoothly;
- expressive and direct gaze paths remain inside +/-20 px X and +/-12 px Y;
- gaze always terminates and settles on the exact bounded destination.

This test does not replace an Arduino firmware compile because it intentionally covers only the two hardware-independent engines.

## 2. Server/source contracts

From `server/`:

```sh
npm test
npm run check
```

The contracts must continue to enforce the layer boundaries documented in `display-animation.md`, including behavior -> expression -> pose/motion -> blink/effect -> renderer separation.

## 3. Firmware compile checkpoint

Compile the final combined branch HEAD once with the confirmed Arduino-ESP32 3.3.10 `esp_sr_16` configuration. Record:

- firmware commit SHA;
- program bytes and percentage;
- static RAM bytes and percentage;
- whether the build completed without warnings/errors relevant to the display modules.

Do not compare only percentages. Record exact bytes so small procedural-engine deltas remain visible.

## 4. Manual face semantics

Before waiting for autonomous behavior, verify the manually addressable visual primitives:

```text
/face_default
/face_curious
/face_closed
/face_detached
/face_sleeping
/face_skeptical
```

Acceptance:

- CURIOUS has strong directional asymmetry: gaze-side eye clearly larger, opposite eye clearly smaller;
- CLOSED reads as proper curved closed eyelids;
- DETACHED preserves the narrow-slit legacy visual and remains effectively stationary;
- SLEEPING uses curved lids, subtle breathing, and procedural moving ZZZ;
- SKEPTICAL is visibly asymmetric;
- switching between open and closed/sleeping poses has no obvious midpoint shape pop.

## 5. Default autonomous life

Return to `/face_default` and observe normal IDLE before judging episodes.

Acceptance:

- gaze never depends on an episode to look alive;
- horizontal strong looks reach visibly farther than the old pre-0.5.1 behavior without clipping;
- vertical looks use the +/-12 px envelope without touching the eye-canvas boundary;
- large saccades may show a tiny anticipation and settle, but should not wobble;
- micro-corrections remain small;
- normal, double, long, and post-saccade blinks still look natural.

## 6. Autonomous expression episodes

USB Serial should make random behavior observable without guessing what happened visually:

```text
[EYES] episode=CURIOUS_SCAN start
[EYES] expr=CURIOUS intensity=...
...
[EYES] episode=CURIOUS_SCAN done
```

Validate that CURIOUS_SCAN, SOCIAL_ATTENTION, ALERT_CHECK, and LOW_ENERGY produce recognizable expression changes rather than only gaze-coordinate changes.

Do not tune an episode because of one random sample. Judge repeated examples and use logs to distinguish selection frequency from rendering quality.

## 7. Drowsy rest

`DROWSY_REST` is intentionally unavailable before the DROWSY inactivity stage (five minutes). Do not lower that threshold in the candidate build merely for validation.

A selected rest sequence should read as:

```text
SLEEPY
  -> downward settle
  -> LONG blink
  -> SLEEPING + ZZZ
  -> SLEEPY wake
  -> centered neutral
```

The sleep hold is only a few seconds. It must not stop the CPU, Wi-Fi, cloud, speaker, voice, clock, or display loop.

## 8. Preemption

During any autonomous episode, especially DROWSY_REST, trigger a real higher-priority context when practical.

Acceptance order remains:

```text
MESSAGE / ECO
ERROR
LISTENING
SPEAKING
THINKING
manual face
autonomous expression
neutral auto
```

The higher-priority context must appear immediately. When it ends, autonomy must restart cleanly rather than resume a half-completed old episode.

A manual face must similarly disable autonomous episodes until `/face_default` is selected again.

## 9. Runtime safety

During the physical run watch existing diagnostics, not only the face:

- `DISPLAY_FRAME` average/worst time;
- free heap and PSRAM;
- speaker underruns/overflows when speaker is exercised;
- voice stability when LISTENING is exercised;
- cloud reconnect/errors;
- USB behavior if connected.

A prettier animation is not accepted if it causes audio, voice, network, or USB regressions.

## Promotion rule

Do not merge the display branch to `main` merely because it compiles. Promote only after host test, server tests, final firmware compile, and physical display/preemption checks all pass on the same final commit SHA.
