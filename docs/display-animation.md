# Newo display animation contract

Newo's face is a procedural character system, not a collection of frame animations.

## Pipeline

```text
runtime signals / internal state
        -> display ownership
        -> behavior episode
        -> expression intent + intensity
        -> EyePose target
        -> pose transition
        -> gaze motion + blink openness
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

## EyePose

`NewoEyePose` is the shared description of what the eyes look like. It owns compact geometry such as left/right width and height, gap, per-eye vertical offsets, top/bottom cuts, openness, and closure style.

Expressions are pose data. They do not own animation timing and do not mutate the persistent manual face.

Expression strength should normally be represented by a bounded `0..100` intensity blended from neutral toward the target pose. Do not create separate `slightly_*`, `very_*`, or threshold-only face variants when intensity can express the same idea.

## Pose motion

`NewoEyePoseEngine` owns transitions between poses. The display may resolve a target every frame, but re-requesting an unchanged target must never restart an in-flight transition.

Use small integer interpolation and the existing display frame loop. Do not allocate memory from the animation path.

Current easing vocabulary is intentionally small:

- linear
- ease-out
- ease-in-out

Add another easing mode only when a visible behavior cannot be expressed cleanly with these.

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

## Blink

The Phase-B bilateral blink scheduler remains the sole owner of normal, double, long, and post-saccade blinks. Winks remain independent one-eye actions.

Expression openness and blink openness compose rather than replace each other:

```text
final openness = expression openness * blink openness
```

A sleepy face must reopen to sleepy, not to fully neutral, after a blink.

## Motion character

Use animation principles sparingly and consistently:

- **anticipation** only for meaningful large actions;
- **overshoot** only when bounded and visually useful;
- **follow-through** as a short settle rather than prolonged wobble;
- **secondary motion** to support a primary action, never to compete with it;
- **asymmetry** when it improves readability of intention.

Strong directional curiosity intentionally enlarges the eye toward the gaze while shrinking the opposite eye. Symmetry is not a requirement for expressions.

## Secondary effects

Effects such as `ZZZ` and sweat are not eye geometry. They are separate procedural overlays selected after the eye state is resolved.

`SLEEPING` is therefore a behavior-like face state composed from:

- CLOSED curved eyelids;
- disabled/random gaze suppression;
- slow breathing motion;
- procedural `ZZZ` secondary animation.

`DETACHED` preserves the former narrow-slit CLOSED visual. `CLOSED` means proper curved closed eyelids.

## Renderer rule

The renderer should be deliberately simple. It receives resolved pose, gaze, blink openness, and secondary effect, then draws once into the existing bounded 1-bit canvases.

The renderer must not know which autonomous episode produced the pose.

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
4. optional behavior orchestration;
5. optional secondary effect if the visual is not eye geometry;
6. contract coverage and physical validation.

If adding a face requires a new renderer branch, first check whether the missing concept belongs in `EyePose` or the secondary-effect layer instead.

## Physical acceptance

Before promoting a display architecture change to `main`:

- `/face_default` visibly keeps continuous gaze and normal blinking;
- horizontal motion remains inside +/-20 px and vertical motion inside +/-12 px;
- operational states preempt autonomy and manual faces correctly;
- the manual face resumes after the operational state ends;
- CLOSED, DETACHED, and SLEEPING remain visually distinct;
- sleeping `ZZZ` stays inside the eye canvas and does not disturb clock/activity regions;
- no display change introduces speaker underruns, voice instability, network stalls, or USB regressions;
- program/DRAM usage is recorded from the final combined compile;
- only the final combined HEAD is flashed for physical validation.
