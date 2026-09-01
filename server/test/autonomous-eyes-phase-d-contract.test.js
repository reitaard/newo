import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("Phase D autonomous behavior is neutral-IDLE owned and reset with face motion", async () => {
  const [header, display] = await Promise.all([
    source("../../Newo/newo_display.h"),
    source("../../Newo/newo_display.cpp"),
  ]);
  assert.match(header, /enum class AutonomousBehavior[\s\S]*WAITING[\s\S]*GLANCE_LEFT[\s\S]*DOUBLE_BLINK[\s\S]*REST_CLOSE/);
  assert.match(display, /bool NewoDisplay::autonomousIdle\(\) const \{\s*return mode_ == NewoDisplayMode::IDLE && faceStyle_ == NewoFaceStyle::NEUTRAL;/);
  assert.match(display, /void NewoDisplay::resetFaceMotion\(uint32_t now\)[\s\S]*resetAutonomousBehavior\(now\);/);
  assert.match(display, /void NewoDisplay::updateAutonomousBehavior\(uint32_t now\) \{\s*if \(!autonomousIdle\(\)\)/);
  assert.match(display, /void NewoDisplay::queuePostSaccadeBlink\(uint32_t now\) \{\s*if \(autonomousBehavior_ != AutonomousBehavior::WAITING/);
});
