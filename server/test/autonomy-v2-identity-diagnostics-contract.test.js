import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("Autonomy V2 procedural eye engine keeps behavior, expression, motion, and effects separated", async () => {
  const [config, display, header, poseHeader, poseSource, poseResolver, cloud, ino, server] = await Promise.all([
    source("../../Newo/newo_config.h"),
    source("../../Newo/newo_display.cpp"),
    source("../../Newo/newo_display.h"),
    source("../../Newo/newo_eye_pose.h"),
    source("../../Newo/newo_eye_pose.cpp"),
    source("../../Newo/newo_display_pose.cpp"),
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

  // Episodes emit semantic expression intent. Only the pose resolver converts
  // that intent to geometry; it must not switch on behavior episodes.
  assert.match(header, /enum class AutonomousExpression[\s\S]*CURIOUS[\s\S]*HAPPY[\s\S]*TIRED[\s\S]*SLEEPY[\s\S]*SURPRISED[\s\S]*CONFUSED[\s\S]*SLEEPING/);
  assert.match(header, /AutonomousExpression autonomousExpression_/);
  assert.match(display, /setAutonomousExpression\(AutonomousExpression::CURIOUS/);
  assert.match(display, /setAutonomousExpression\(AutonomousExpression::HAPPY/);
  assert.match(display, /setAutonomousExpression\(AutonomousExpression::SLEEPING, 100\)/);
  assert.match(poseResolver, /switch \(autonomousExpression_\)/);
  assert.doesNotMatch(poseResolver, /switch \(autonomousEpisode_\)/);
  assert.match(poseResolver, /NewoEyePoseEngine::blend\(neutral, target, autonomousExpressionIntensity_\)/);

  // Transient shake/bounce/stretch/breathing is resolved as data outside the
  // renderer. drawFaceFrame consumes the overlay but contains no semantic
  // expression/episode motion branches and no trigonometric animation logic.
  assert.match(header, /struct EyeMotionOverlay/);
  assert.match(header, /EyeMotionOverlay resolveEyeMotionOverlay/);
  assert.match(poseResolver, /NewoDisplay::EyeMotionOverlay NewoDisplay::resolveEyeMotionOverlay/);
  assert.match(display, /const EyeMotionOverlay motion = resolveEyeMotionOverlay\(now, activeMode\)/);
  assert.match(renderer[1], /motion\.xOffset/);
  assert.match(renderer[1], /motion\.yOffset/);
  assert.match(renderer[1], /motion\.leftWidthDelta/);
  assert.match(renderer[1], /motion\.leftHeightDelta/);
  assert.doesNotMatch(renderer[1], /NewoFaceStyle::CONFUSED|NewoFaceStyle::LAUGH/);
  assert.doesNotMatch(renderer[1], /AutonomousEpisode::ALERT_CHECK/);
  assert.doesNotMatch(renderer[1], /NewoDisplayMode::THINKING|NewoDisplayMode::ERROR/);
  assert.doesNotMatch(renderer[1], /sinf\(/);

  // Renderer consumes resolved pose + gaze + blink + motion + effects.
  assert.match(header, /NewoEyePoseEngine eyePoseEngine_/);
  assert.match(renderer[1], /resolveEyePose\(now, activeMode\)/);
  assert.match(renderer[1], /combinedOpen = static_cast<uint16_t>\(pose\.openness\) \* blinkOpen/);
  assert.match(renderer[1], /applyResolvedPoseCuts/);
  assert.match(renderer[1], /drawSecondaryEffect/);
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
  assert.match(header, /SecondaryEffect : uint8_t \{ NONE, ZZZ, SWEAT \}/);
  assert.match(poseResolver, /case NewoFaceStyle::CLOSED:[\s\S]*NewoEyeClosureStyle::CURVED/);
  assert.match(poseResolver, /case NewoFaceStyle::DETACHED:[\s\S]*leftHeight = pose\.rightHeight = 4/);
  assert.match(poseResolver, /case NewoFaceStyle::SLEEPING:[\s\S]*NewoEyeClosureStyle::CURVED/);
  assert.match(poseResolver, /case NewoFaceStyle::SKEPTICAL:/);
  assert.match(poseResolver, /autonomousExpression_ == AutonomousExpression::SLEEPING \? SecondaryEffect::ZZZ/);
  assert.match(poseResolver, /drawZ\(/);
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
