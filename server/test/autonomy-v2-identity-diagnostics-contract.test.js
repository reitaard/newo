import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("Autonomy V2 procedural eye engine keeps ownership, motion, and effects separated", async () => {
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

  // Renderer consumes resolved pose + gaze + blink; expressions are not drawn by episode cases.
  assert.match(header, /NewoEyePoseEngine eyePoseEngine_/);
  assert.match(display, /resolveEyePose\(now, activeMode\)/);
  assert.match(display, /combinedOpen = static_cast<uint16_t>\(pose\.openness\) \* blinkOpen/);
  assert.match(display, /applyResolvedPoseCuts/);
  assert.match(display, /drawSecondaryEffect/);
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
  assert.match(poseResolver, /NewoEyePoseEngine::blend\(neutral, target/);

  // Face semantics and secondary effects are explicit and independent.
  assert.match(header, /DETACHED, SLEEPING, SKEPTICAL/);
  assert.match(header, /SecondaryEffect : uint8_t \{ NONE, ZZZ, SWEAT \}/);
  assert.match(poseResolver, /case NewoFaceStyle::CLOSED:[\s\S]*NewoEyeClosureStyle::CURVED/);
  assert.match(poseResolver, /case NewoFaceStyle::DETACHED:[\s\S]*leftHeight = pose\.rightHeight = 4/);
  assert.match(poseResolver, /case NewoFaceStyle::SLEEPING:[\s\S]*NewoEyeClosureStyle::CURVED/);
  assert.match(poseResolver, /case NewoFaceStyle::SKEPTICAL:/);
  assert.match(poseResolver, /SecondaryEffect::ZZZ/);
  assert.match(poseResolver, /drawZ\(/);
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
});
