import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("Autonomy V2 procedural eye engine keeps behavior, expression, motion, blink, and effects separated", async () => {
  const [config, display, header, poseHeader, poseSource, poseResolver, blinkSource, cloud, ino, server] = await Promise.all([
    source("../../Newo/newo_config.h"),
    source("../../Newo/newo_display.cpp"),
    source("../../Newo/newo_display.h"),
    source("../../Newo/newo_eye_pose.h"),
    source("../../Newo/newo_eye_pose.cpp"),
    source("../../Newo/newo_display_pose.cpp"),
    source("../../Newo/newo_display_blink.cpp"),
    source("../../Newo/newo_cloud.cpp"),
    source("../../Newo/Newo.ino"),
    source("../src/index.js"),
  ]);
  const renderer = display.match(/void NewoDisplay::drawFaceFrame\(uint32_t now\) \{([\s\S]*?)\n\}\n\nvoid NewoDisplay::blitMonoCanvasFast/);
  assert.ok(renderer, "drawFaceFrame body should be discoverable for renderer-boundary checks");

  assert.match(config, /FIRMWARE_VERSION\[\] = "0\.5\.2-dev"/);
  assert.match(config, /AUTONOMY_REVISION = 2/);
  assert.match(ino, /Firmware: %s/);
  assert.match(ino, /Autonomy: V%u/);
  assert.match(cloud, /doc\["autonomy_revision"\] = NewoConfig::AUTONOMY_REVISION/);
  assert.match(server, /autonomy_revision: z\.number\(\)\.int\(\)\.nonnegative\(\)\.optional\(\)/);

  // Pose is data; transition mechanics live outside the display behavior scheduler.
  assert.match(poseHeader, /struct NewoEyePose/);
  assert.match(poseHeader, /leftWidth/);
  assert.match(poseHeader, /rightHeight/);
  assert.match(poseHeader, /openness/);
  assert.match(poseHeader, /NewoEyeClosureStyle/);
  assert.match(poseHeader, /class NewoEyePoseEngine/);
  assert.match(poseSource, /transitionTo/);
  assert.match(poseSource, /EASE_IN_OUT/);
  assert.match(poseSource, /blend\(const NewoEyePose& neutral/);
  assert.doesNotMatch(poseSource, /malloc|new\s|std::vector|std::map/);

  // Discrete curved/filled shape handoff is direction-aware: closing switches
  // late and waking switches early, avoiding a visible midpoint shape pop.
  assert.match(poseSource, /interpolateClosureStyle/);
  assert.match(poseSource, /to == NewoEyeClosureStyle::CURVED[\s\S]*permille < 850/);
  assert.match(poseSource, /return permille < 150 \? from : to/);
  assert.match(poseSource, /current_\.closureStyle = interpolateClosureStyle/);

  // Episodes emit semantic expression intent. Only the pose resolver converts
  // that intent to geometry or autonomous motion; it must not inspect behavior episodes.
  assert.match(header, /enum class AutonomousExpression[\s\S]*CURIOUS[\s\S]*HAPPY[\s\S]*TIRED[\s\S]*SLEEPY[\s\S]*SURPRISED[\s\S]*CONFUSED[\s\S]*SLEEPING/);
  assert.match(header, /AutonomousExpression autonomousExpression_/);
  assert.match(display, /setAutonomousExpression\(AutonomousExpression::CURIOUS/);
  assert.match(display, /setAutonomousExpression\(AutonomousExpression::HAPPY/);
  assert.match(display, /setAutonomousExpression\(AutonomousExpression::SLEEPING, 100\)/);
  assert.match(poseResolver, /switch \(autonomousExpression_\)/);
  assert.doesNotMatch(poseResolver, /autonomousEpisode_/);
  assert.match(poseResolver, /autonomousExpression_ == AutonomousExpression::CONFUSED/);
  assert.match(poseResolver, /NewoEyePoseEngine::blend\(neutral, target, autonomousExpressionIntensity_\)/);

  // Transient shake/bounce/stretch/breathing is resolved as data outside the
  // renderer. drawFaceFrame consumes the overlay but contains no semantic
  // expression/episode motion branches and no trigonometric animation logic.
  assert.match(header, /struct EyeMotionOverlay/);
  assert.match(header, /EyeMotionOverlay resolveEyeMotionOverlay/);
  assert.match(poseResolver, /NewoDisplay::EyeMotionOverlay NewoDisplay::resolveEyeMotionOverlay/);
  assert.match(renderer[1], /motion\.xOffset/);
  assert.match(renderer[1], /motion\.yOffset/);
  assert.match(renderer[1], /motion\.leftWidthDelta/);
  assert.match(renderer[1], /motion\.leftHeightDelta/);
  assert.doesNotMatch(renderer[1], /NewoFaceStyle::CONFUSED|NewoFaceStyle::LAUGH/);
  assert.doesNotMatch(renderer[1], /AutonomousEpisode::ALERT_CHECK/);
  assert.doesNotMatch(renderer[1], /NewoDisplayMode::THINKING|NewoDisplayMode::ERROR/);
  assert.doesNotMatch(renderer[1], /sinf\(/);

  // Renderer consumes resolved pose + gaze + blink + motion + effects. Phase B
  // scheduling lives in the blink service; the renderer only services and uses
  // the already-resolved blink state.
  assert.match(header, /NewoEyePoseEngine eyePoseEngine_/);
  assert.match(renderer[1], /updateBlinkBeforeFrame\(now, activeMode\)/);
  assert.match(renderer[1], /advanceBlinkAfterFrame\(now\)/);
  assert.match(renderer[1], /resolveEyePose\(now, activeMode\)/);
  assert.match(renderer[1], /combinedOpen = static_cast<uint16_t>\(pose\.openness\) \* blinkOpen/);
  assert.match(renderer[1], /applyResolvedPoseCuts/);
  assert.match(renderer[1], /drawSecondaryEffect/);
  assert.doesNotMatch(renderer[1], /startBilateralBlink|blinkSchedulerState_|nextBlinkMs_|nextWinkMs_/);
  assert.match(blinkSource, /void NewoDisplay::updateBlinkBeforeFrame/);
  assert.match(blinkSource, /void NewoDisplay::advanceBlinkAfterFrame/);
  assert.match(blinkSource, /AutonomousEpisode::DROWSY_REST/);
  assert.match(blinkSource, /startBilateralBlink\(false, kForceLongBlink\)/);
  assert.match(blinkSource, /BlinkSchedulerState::DOUBLE_SECOND/);
  assert.doesNotMatch(display, /applyEyeExpression\(/);

  // Continuous default gaze remains wide but bounded; vertical travel is the requested small increase.
  assert.match(display, /kAutonomousGazeHardX = 20/);
  assert.match(display, /kAutonomousGazeHardY = 12/);
  assert.match(display, /sideRangeX = energy_ < 55 \? 16 : energy_ > 80 \? 20 : 18/);
  assert.match(display, /random\(-12, -3\)/);
  assert.match(display, /candidate >= -kAutonomousGazeHardY && candidate <= kAutonomousGazeHardY/);

  // Directional curiosity makes one eye substantially larger and the other smaller.
  assert.match(poseResolver, /pose\.leftWidth = 50/);
  assert.match(poseResolver, /pose\.leftHeight = 30/);
  assert.match(poseResolver, /pose\.rightWidth = 68/);
  assert.match(poseResolver, /pose\.rightHeight = 50/);

  // CLOSED/DETACHED/SLEEPING semantics are explicit; sleep effects remain
  // independent and DROWSY_REST is an orchestrated behavior, not a face renderer.
  assert.match(header, /DETACHED, SLEEPING, SKEPTICAL/);
  assert.match(header, /DROWSY_REST/);
  assert.match(poseResolver, /case NewoFaceStyle::CLOSED:[\s\S]*NewoEyeClosureStyle::CURVED/);
  assert.match(poseResolver, /case NewoFaceStyle::DETACHED:[\s\S]*leftHeight = pose\.rightHeight = 4/);
  assert.match(poseResolver, /faceStyle_ == NewoFaceStyle::DETACHED[\s\S]*overlay\.xOffset -= gazeX_[\s\S]*overlay\.yOffset -= gazeY_/);
  assert.match(poseResolver, /case NewoFaceStyle::SLEEPING:[\s\S]*NewoEyeClosureStyle::CURVED/);

  const skeptical = poseResolver.match(/case NewoFaceStyle::SKEPTICAL: \{([\s\S]*?)\n      break;\n    \}/);
  assert.ok(skeptical, "skeptical pose should be a directional pose block");
  assert.match(skeptical[1], /direction = gazeTargetX_ < -5 \? -1 : gazeTargetX_ > 5 \? 1/);
  const skepticalRight = skeptical[1].match(/if \(direction > 0\) \{([\s\S]*?)\n      \} else/);
  const skepticalLeft = skeptical[1].match(/else \{([\s\S]*?)\n      \}/);
  assert.ok(skepticalRight && skepticalLeft, "skeptical pose should mirror both gaze directions");
  assert.match(skepticalRight[1], /leftWidth = 50[\s\S]*leftHeight = 24[\s\S]*leftTopCut = -12[\s\S]*rightWidth = 62[\s\S]*rightHeight = 38/);
  assert.doesNotMatch(skepticalRight[1], /rightTopCut/);
  assert.match(skepticalLeft[1], /leftWidth = 62[\s\S]*leftHeight = 38[\s\S]*rightWidth = 50[\s\S]*rightHeight = 24[\s\S]*rightTopCut = -12/);
  assert.doesNotMatch(skepticalLeft[1], /leftTopCut/);

  // Phase E generalizes the secondary-effect layer without coupling decorative
  // symbols to face poses. The approved ZZZ geometry stays intact.
  assert.match(header, /enum class NewoSecondaryEffect[\s\S]*NONE[\s\S]*ZZZ[\s\S]*QUESTION[\s\S]*EXCLAMATION[\s\S]*SURPRISE_MARK[\s\S]*ELLIPSIS[\s\S]*SWEAT/);
  assert.match(header, /bool setSecondaryEffect\(NewoSecondaryEffect effect, uint32_t durationMs = 6'000\)/);
  assert.match(header, /secondaryEffectOverride_ = NewoSecondaryEffect::NONE/);
  assert.match(header, /secondaryEffectStartedMs_/);
  assert.match(header, /secondaryEffectUntilMs_/);
  assert.match(poseResolver, /bool NewoDisplay::setSecondaryEffect\(NewoSecondaryEffect effect, uint32_t durationMs\)/);
  assert.match(poseResolver, /durationMs < 500 \|\| durationMs > 15'000/);
  assert.match(poseResolver, /if \(mode != NewoDisplayMode::IDLE\) return NewoSecondaryEffect::NONE/);
  assert.match(poseResolver, /static_cast<int32_t>\(now - secondaryEffectUntilMs_\) < 0/);
  assert.match(poseResolver, /autonomousExpression_ == AutonomousExpression::SLEEPING \? NewoSecondaryEffect::ZZZ/);
  assert.match(poseResolver, /void NewoDisplay::drawZ[\s\S]*if \(size < 5\) return;[\s\S]*fillRect\(x, y, size \+ 1, 2, 1\)/);
  assert.match(poseResolver, /effect == NewoSecondaryEffect::ZZZ[\s\S]*index < 2[\s\S]*index == 0 \? 7 : 10/);
  for (const effect of ["QUESTION", "EXCLAMATION", "SURPRISE_MARK", "ELLIPSIS", "SWEAT"]) {
    assert.match(poseResolver, new RegExp(`effect == NewoSecondaryEffect::${effect}`), `missing procedural effect: ${effect}`);
  }
  const effectResolver = poseResolver.match(/NewoSecondaryEffect NewoDisplay::secondaryEffectFor\(uint32_t now, NewoDisplayMode mode\) const \{([\s\S]*?)\n\}/);
  assert.ok(effectResolver, "secondary effect resolver should be discoverable");
  assert.doesNotMatch(effectResolver[1], /CURIOUS.*QUESTION|SURPRISED.*EXCLAMATION|CONFUSED.*QUESTION/);

  // Phase E transport exposes a silent manual test path while keeping effects
  // independent from the face command namespace.
  assert.match(cloud, /strcmp\(mode, "effect"\) == 0/);
  for (const [name, effect] of [
    ["zzz", "ZZZ"], ["question", "QUESTION"], ["exclamation", "EXCLAMATION"],
    ["surprise", "SURPRISE_MARK"], ["ellipsis", "ELLIPSIS"], ["sweat", "SWEAT"],
  ]) {
    assert.match(cloud, new RegExp(`strcmp\\(text, "${name}"\\).*NewoSecondaryEffect::${effect}`));
  }
  assert.match(cloud, /display_\.setSecondaryEffect\(/);
  assert.match(server, /const SECONDARY_EFFECTS = \["none", "question", "exclamation", "surprise", "ellipsis", "sweat", "zzz"\]/);
  assert.match(server, /\{ command: "effect", description: "Test a secondary effect" \}/);
  assert.match(server, /sendDeviceRequest\("display_set", "display_ack", \{ mode: "effect", text: input, duration_ms: durationMs \}/);
  assert.match(server, /bot\.command\(\["effect", "fx"\], handleEffectCommand\)/);
  const effectHandler = server.match(/async function handleEffectCommand\(ctx\) \{([\s\S]*?)\n\}/);
  assert.ok(effectHandler, "Telegram effect handler should be discoverable");
  assert.match(effectHandler[1], /newoSpeak: false/);

  assert.match(display, /case AutonomousEpisode::DROWSY_REST:[\s\S]*AutonomousExpression::SLEEPY/);
  assert.match(display, /AutonomousEpisode::DROWSY_REST[\s\S]*AutonomousExpression::SLEEPING, 100/);
  assert.match(cloud, /strcmp\(mode, "detached"\)/);
  assert.match(cloud, /strcmp\(mode, "sleeping"\)/);
  assert.match(cloud, /strcmp\(mode, "skeptical"\)/);

  // Phase B still owns bilateral blink sequencing and bounded diagnostics remain.
  assert.match(display, /scheduleNextBilateralBlink/);
  assert.match(display, /startBilateralBlink/);
  assert.match(display, /eyeMeaningfulGazeEvents_/);
  assert.match(display, /eyeDoubleBlinkEvents_/);
  assert.match(display, /eyeEpisodeCompletions_/);
  assert.match(display, /\[EYES_STATS\]/);
  assert.match(display, /\[EYES\] expr=%s intensity=%u/);
});
