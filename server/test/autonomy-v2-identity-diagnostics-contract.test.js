import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("Autonomy V2 has a distinguishable identity and bounded eye diagnostics", async () => {
  const [config, display, cloud, ino, server, validation] = await Promise.all([
    source("../../Newo/newo_config.h"),
    source("../../Newo/newo_display.cpp"),
    source("../../Newo/newo_cloud.cpp"),
    source("../../Newo/Newo.ino"),
    source("../src/index.js"),
    source("../../Newo/PHYSICAL_OPUS_VALIDATION.md"),
  ]);

  assert.match(config, /FIRMWARE_VERSION\[\] = "0\.5\.0-dev"/);
  assert.match(config, /AUTONOMY_REVISION = 2/);
  assert.match(ino, /Firmware: %s/);
  assert.match(ino, /Autonomy: V%u/);
  assert.match(cloud, /doc\["autonomy_revision"\] = NewoConfig::AUTONOMY_REVISION/);
  assert.match(server, /autonomy_revision: z\.number\(\)\.int\(\)\.nonnegative\(\)\.optional\(\)/);
  assert.match(server, /autonomy_revision: device\.hello\?\.autonomy_revision \?\? null/);
  assert.match(display, /eyeContextChanges_/);
  assert.match(display, /eyeMeaningfulGazeEvents_/);
  assert.match(display, /eyeDoubleBlinkEvents_/);
  assert.match(display, /eyeEpisodeCompletions_/);
  assert.match(display, /\[EYES_STATS\]/);
  assert.match(display, /nextAutonomousStateLogMs_ = now \+ kAutonomousStateLogMs/);
  assert.match(validation, /firmware `0\.5\.0-dev`/);
});
