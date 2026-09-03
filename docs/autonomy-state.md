# Newo autonomy state contract

Newo's internal character state is slow semantic context for behavior selection. It is not a renderer, expression selector, animation scheduler, or persistence system.

## Ownership

`NewoAutonomyState` is the single runtime owner of:

- energy;
- curiosity;
- social engagement;
- stress;
- last interaction time;
- energy drift/recovery cadence.

The display class may read these values when choosing behavior, gaze timing, blink timing, or expression intensity, but it must not keep shadow copies of the same state.

Fatigue is derived rather than independently stored:

```text
fatigue = 100 - energy
```

This prevents energy and fatigue from drifting out of agreement.

## Inactivity stages

The state engine exposes one semantic inactivity stage:

```text
ACTIVE   < 2 minutes
RELAXED  >= 2 minutes
DROWSY   >= 5 minutes
```

Stages are derived from `lastInteractionMs`; callers must not recreate the thresholds ad hoc.

An interaction immediately returns the inactivity stage to ACTIVE.

## Energy lifecycle

Energy changes deliberately rather than falling on every state tick.

After 30 seconds without interaction, idle energy may drift downward using bounded stage-specific cadence and floors:

```text
ACTIVE   one step / 36 s   floor 62
RELAXED  one step / 24 s   floor 50
DROWSY   one step / 18 s   floor 35
```

Sustained LISTENING, THINKING, or SPEAKING keeps inactivity fresh and may recover energy slowly, at most one step every 18 seconds, toward a bounded engagement target. Entering those operational contexts may still apply the existing small immediate interaction gain.

The purpose is character continuity, not a battery simulation. Energy must never control real ESP32 power state or shut down voice, Wi-Fi, speaker, USB, or cloud resources.

## Curiosity, social, and stress

Curiosity relaxes one step at a time toward its baseline of 42.

Social engagement rises gradually toward context-specific targets during active interaction and drifts toward an idle floor when Newo is not engaged.

Errors raise stress by a bounded amount. Stress then decays gradually. Stress is behavioral context only; it is not a substitute for actual error telemetry.

All state values remain bounded to `0..100`.

## Layer boundary

The permanent direction is:

```text
runtime events
    -> NewoAutonomyState
    -> behavior selection / behavior episode
    -> expression + gaze + blink + presentation requests
    -> pose / motion / effects / captions
    -> renderer
```

`NewoAutonomyState` must never contain or select:

- `AutonomousExpression`;
- `NewoEyePose`;
- `NewoSecondaryEffect`;
- `NewoFaceCaption`;
- gaze coordinates;
- blink phases;
- renderer geometry.

A future composition/orchestration layer may use internal state to decide whether a reaction should be subtle, energetic, tired, curious, or stressed, but the primitives remain independent and reusable.

## Resource contract

The state engine stays host portable and fixed-size:

- no `Arduino.h` dependency;
- no heap allocation;
- no dynamic STL containers;
- no task/thread;
- no persistence writes on the animation path;
- no display buffers.

`tools/autonomy-state-host-test.cpp` validates stage thresholds, bounded idle fatigue, interaction recovery, saturation, stress decay, and curiosity decay under the same dependency-free host CI used by the eye engines.

## Diagnostics

`[EYES_STATS]` exposes the resolved semantic state for physical validation:

```text
energy=<0..100>
fatigue=<0..100>
curiosity=<0..100>
social=<0..100>
stress=<0..100>
stage=ACTIVE|RELAXED|DROWSY
```

These values are diagnostic inputs to behavior. They are not promises that a specific face or caption must appear at a fixed threshold.
