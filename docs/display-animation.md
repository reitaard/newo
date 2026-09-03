# Newo display animation contract

Newo's face is a procedural character system, not a collection of frame animations.

## Pipeline

```text
runtime signals / internal state
        -> display ownership
        -> behavior episode
        -> AutonomousExpression + intensity
        -> EyePose target
        -> pose transition
        -> gaze motion
        -> resolved EyeMotionOverlay
        -> Phase-B blink state
        -> secondary effect
        -> renderer
```

Each layer has one responsibility. A behavior may request an expression, gaze, blink, hold, or secondary effect, but it must not draw eye geometry directly.

## Ownership

Full-screen MESSAGE/ECO content owns the display first. Face ownership then resolves in this order:

```text
ERROR
LISTENING
SPEAKING
THINKING
manual face override
autonomous expression
neutral autonomous face
```

Operational contexts temporarily supersede a manual face but must not erase it. `/face_default` enables autonomous face behavior; other `/face_*` commands are persistent manual overrides until replaced.

An operational context also immediately cancels any active autonomous episode and expression intent. When the device returns to IDLE_AUTO, autonomy starts again from a neutral, freshly scheduled state instead of resuming half-way through an old animation.

## Autonomous expression intent

Behavior episodes never select eye geometry directly. They emit one compact semantic intent plus a bounded intensity:

```text
NONE
CURIOUS
HAPPY
TIRED
SLEEPY
SURPRISED
CONFUSED
SLEEPING
```

`AutonomousExpression` is volatile runtime state and never mutates `faceStyle_`. The pose/motion resolver is the only layer that converts an autonomous expression into eye geometry or expression-specific secondary motion. It must never inspect `autonomousEpisode_` to decide what an expression looks like.

This separation is a permanent design rule:

```text
behavior episode != expression != pose != motion != renderer
```

Changing what CURIOUS looks like must not require editing `CURIOUS_SCAN`. Changing the timing of `CURIOUS_SCAN` must not require editing the CURIOUS pose.

Expression strength should normally be represented by a bounded `0..100` intensity blended from neutral toward the target pose. Do not create separate `slightly_*`, `very_*`, or threshold-only face variants when intensity can express the same idea.

## EyePose

`NewoEyePose` is the shared description of what the eyes look like. It owns compact geometry such as left/right width and height, gap, per-eye vertical offsets, top/bottom cuts, openness, and closure style.

Expressions are pose data. They do not own animation timing and do not mutate the persistent manual face.

## Pose motion

`NewoEyePoseEngine` owns transitions between poses. The display may resolve a target every frame, but re-requesting an unchanged target must never restart an in-flight transition.

Use small integer interpolation and the existing display frame loop. Do not allocate memory from the animation path.

Current easing vocabulary is intentionally small:

- linear
- ease-out
- ease-in-out

Add another easing mode only when a visible behavior cannot be expressed cleanly with these.

Closure style is discrete even though eye geometry/openness is continuous. Therefore filled-to-curved transitions use a direction-aware handoff: closing keeps filled geometry until roughly 85% of the morph, while waking returns to filled geometry around 15%. Do not restore a generic midpoint switch; it produces a visible shape pop.

## Gaze

Gaze is independent of expression. A CURIOUS pose can therefore look left, right, up, or down without creating four different expression implementations.

Default autonomous IDLE keeps continuous gaze life even when no behavior episode is active.

Current autonomous envelope:

```text
horizontal hard limit: +/-20 px
vertical hard limit:   +/-12 px
```

A destination is chosen by the behavior/gaze scheduler. `NewoGazeMotion` owns how a large saccade reaches it.

Large autonomous saccades may use:

1. a tiny bounded anticipation counter-motion;
2. fast travel;
3. at most a small bounded overshoot where canvas room exists;
4. settle back to the requested destination.

Small gaze changes and micro-corrections must remain direct and should not be inflated into dramatic motion.

The approved hard envelope must be respected during anticipation and overshoot, not only at the final destination.

Manual CLOSED, SLEEPING, and DETACHED do not use ordinary random gaze. DETACHED specifically preserves the former CLOSED semantics: its narrow slit eyes remain effectively stationary even though the generic manual gaze state continues to exist underneath.

## Resolved motion overlay

Transient shake, bounce, breathing, and small squash/stretch belong in `EyeMotionOverlay`, not in `drawFaceFrame()`.

The overlay is compact resolved data:

```text
x/y offset
left/right width delta
left/right height delta
```

Operational states, manual faces, expression intent, or gaze state may contribute to the overlay before rendering. The renderer consumes the result without knowing why it was requested.

Autonomous overlay logic must follow expression intent rather than behavior episode identity. For example, the autonomous confused shake is attached to `AutonomousExpression::CONFUSED`, not to `ALERT_CHECK`.

## Blink

The Phase-B bilateral blink scheduler remains the sole owner of normal, double, long, and post-saccade blinks. Winks remain independent one-eye actions.

Expression openness and blink openness compose rather than replace each other:

```text
final openness = expression openness * blink openness
```

A sleepy face must reopen to sleepy, not to fully neutral, after a blink.

Behaviors may request an existing blink type, but they do not implement their own eyelid animation. `LOW_ENERGY` and `DROWSY_REST`, for example, request the existing long-blink path.

`drawFaceFrame()` must not decide when blinks/winks start or advance the Phase-B scheduler directly. It services `updateBlinkBeforeFrame()`, uses the resolved blink state while drawing, then calls `advanceBlinkAfterFrame()`. Scheduling/phase logic belongs outside the renderer.

## Motion character

Use animation principles sparingly and consistently:

- **anticipation** only for meaningful large actions;
- **overshoot** only when bounded and visually useful;
- **follow-through** as a short settle rather than prolonged wobble;
- **secondary motion** to support a primary action, never to compete with it;
- **asymmetry** when it improves readability of intention.

Strong directional curiosity intentionally enlarges the eye toward the gaze while shrinking the opposite eye. Symmetry is not a requirement for expressions.

## Drowsy rest behavior

`DROWSY_REST` is an autonomous behavior, not another static face. It is eligible only in the DROWSY inactivity stage (currently after five minutes without interaction) and has a deliberately modest selection weight so Newo does not constantly fall asleep.

The sequence is:

```text
SLEEPY + slight downward gaze
        -> existing LONG blink
        -> SLEEPING + procedural ZZZ + slow breathing
        -> SLEEPY wake
        -> neutral + centered gaze
        -> finish episode
```

The sleeping hold is bounded to a few seconds. It is not a persistent power/sleep mode and does not stop the ESP32, display loop, network, voice, or speaker subsystems.

Any higher-priority display context cancels the episode immediately. LISTENING, THINKING, SPEAKING, ERROR, MESSAGE, ECO, or a manual face command must never wait for the rest animation to finish.

## Secondary effects

Effects such as `ZZZ` and sweat are not eye geometry. They are separate procedural overlays selected after the eye state is resolved.

Manual `SLEEPING` and autonomous `AutonomousExpression::SLEEPING` both reuse the same procedural sleep presentation:

- CLOSED curved eyelids;
- centered/suppressed random gaze during the sleep hold;
- slow breathing motion;
- procedural `ZZZ` secondary animation.

`DETACHED` preserves the former narrow-slit CLOSED visual. `CLOSED` means proper curved closed eyelids.

## Renderer rule

The renderer should be deliberately simple. It receives resolved pose, gaze, `EyeMotionOverlay`, blink openness, and secondary effect, then draws once into the existing bounded 1-bit canvases.

The renderer must not know which autonomous episode or semantic expression produced those resolved values. It must contain no expression-specific shake/bounce/breathing logic and no blink scheduling logic.

Behavior-specific motion may affect gaze scheduling, but new eye geometry must never be added as an `if (episode == ...)` renderer branch.

## Resource rules

Do not add these for face animation:

- bitmap/GIF animation frames;
- a full RGB framebuffer;
- a new animation task;
- expression-specific heap allocation;
- dynamic STL containers in the frame loop;
- a new animation library when the existing procedural engine can express the behavior.

Prefer:

- fixed-size structs;
- integer parameters;
- small procedural geometry;
- the existing 20 FPS display loop;
- the existing 1-bit eye/activity canvases;
- bounded state machines.

Voice, speaker, Wi-Fi, USB, and cloud timing have priority over decorative animation.

## Face extension checklist

A new expression should normally require only:

1. a new semantic face identifier when manual access is desired;
2. a pose definition in the pose resolver;
3. optional intensity mapping from internal state;
4. optional resolved motion contribution;
5. optional behavior orchestration;
6. optional secondary effect if the visual is not eye geometry;
7. contract coverage and physical validation.

A new autonomous behavior should normally require only:

1. an episode/state-machine entry;
2. expression-intent requests;
3. gaze/blink/hold orchestration;
4. no renderer geometry changes.

If adding a face or behavior requires a new renderer branch, first check whether the missing concept belongs in `EyePose`, `NewoGazeMotion`, `EyeMotionOverlay`, the blink scheduler, or the secondary-effect layer instead.

## Physical acceptance

Before promoting a display architecture change to `main`:

- `/face_default` visibly keeps continuous gaze and normal blinking;
- horizontal motion remains inside +/-20 px and vertical motion inside +/-12 px;
- operational states preempt autonomy and manual faces correctly;
- the manual face resumes after the operational state ends;
- autonomous expression intent clears on preemption rather than resuming stale;
- CLOSED, DETACHED, and SLEEPING remain visually distinct;
- DETACHED remains visually stationary rather than inheriting ordinary manual gaze;
- `DROWSY_REST` visibly performs sleepy -> sleep -> wake without becoming persistent;
- sleeping `ZZZ` stays inside the eye canvas and does not disturb clock/activity regions;
- no display change introduces speaker underruns, voice instability, network stalls, or USB regressions;
- program/DRAM usage is recorded from the final combined compile;
- only the final combined HEAD is flashed for physical validation.
