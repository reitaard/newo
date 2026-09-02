import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("Autonomy V2.1 has a distinguishable identity, wider gaze, and visible episode expressions", async () => {
  const [config, display, cloud, ino, server] = await Promise.all([
    source("../../Newo/newo_config.h"),
    source("../../Newo/newo_display.cpp"),
    source("../../Newo/newo_cloud.cpp"),
    source("../../Newo/Newo.ino"),
    source("../src/index.js"),
  ]);

  assert.match(config, /FIRMWARE_VERSION\[\] = "0\.5\.1-dev"/);
  assert.match(config, /AUTONOMY_REVISION = 2/);
  assert.match(ino, /Firmware: %s/);
  assert.match(ino, /Autonomy: V%u/);
  assert.match(cloud, /doc\["autonomy_revision"\] = NewoConfig::AUTONOMY_REVISION/);
  assert.match(server, /autonomy_revision: z\.number\(\)\.int\(\)\.nonnegative\(\)\.optional\(\)/);
  assert.match(server, /autonomy_revision: device\.hello\?\.autonomy_revision \?\? null/);

  assert.match(display, /kAutonomousEpisodeMinMs = 8'000/);
  assert.match(display, /kAutonomousEpisodeMaxMs = 18'000/);
  assert.match(display, /kAutonomousGazeHardX = 20/);
  assert.match(display, /sideRangeX = energy_ < 55 \? 16 : energy_ > 80 \? 20 : 18/);
  assert.match(display, /random\(12, sideRangeX \+ 1\)/);
  assert.match(display, /candidate >= -kAutonomousGazeHardX && candidate <= kAutonomousGazeHardX/);

  assert.match(display, /autonomousEpisode_ == AutonomousEpisode::LOW_ENERGY/);
  assert.match(display, /autonomousEpisode_ == AutonomousEpisode::SOCIAL_ATTENTION/);
  assert.match(display, /case AutonomousEpisode::CURIOUS_SCAN:[\s\S]*leftW = rightW = 59/);
  assert.match(display, /case AutonomousEpisode::SOCIAL_ATTENTION:[\s\S]*leftW = rightW = 64/);
  assert.match(display, /case AutonomousEpisode::ALERT_CHECK:[\s\S]*baseHeight = 54/);
  assert.match(display, /autonomousEpisode_ == AutonomousEpisode::ALERT_CHECK && autonomousEpisodeDirection_ < 0/);
  assert.match(display, /const int16_t lift = 8/);

  assert.match(display, /eyeContextChanges_/);
  assert.match(display, /eyeMeaningfulGazeEvents_/);
  assert.match(display, /eyeDoubleBlinkEvents_/);
  assert.match(display, /eyeEpisodeCompletions_/);
  assert.match(display, /\[EYES_STATS\]/);
  assert.match(display, /nextAutonomousStateLogMs_ = now \+ kAutonomousStateLogMs/);
});
