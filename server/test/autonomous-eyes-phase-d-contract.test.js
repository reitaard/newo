import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("Autonomy V2 is neutral-IDLE owned and episode driven", async () => {
  const [header, display] = await Promise.all([
    source("../../Newo/newo_display.h"),
    source("../../Newo/newo_display.cpp"),
  ]);
  assert.match(header, /enum class AutonomousEpisode[\s\S]*WAITING[\s\S]*CURIOUS_SCAN[\s\S]*LOW_ENERGY[\s\S]*SOCIAL_ATTENTION[\s\S]*ALERT_CHECK/);
  assert.match(display, /bool NewoDisplay::autonomousIdle\(\) const \{\s*return effectiveMode\(millis\(\)\) == NewoDisplayMode::IDLE && autoFaceEnabled_;/);
  assert.match(display, /void NewoDisplay::resetFaceMotion\(uint32_t now\)[\s\S]*resetAutonomousEpisode\(now\);/);
  assert.match(display, /constexpr uint32_t kAutonomousEpisodeMinMs = 10'000;[\s\S]*constexpr uint32_t kAutonomousEpisodeMaxMs = 20'000;/);
  assert.match(display, /void NewoDisplay::updateAutonomousEpisode\(uint32_t now\)[\s\S]*chooseAutonomousEpisode\(now\)/);
  assert.match(display, /void NewoDisplay::chooseAutonomousGazeTarget\(\)[\s\S]*random\(10, sideRangeX \+ 1\)/);
  assert.match(display, /startBilateralBlink\(false, kForceLongBlink\)/);
  assert.match(display, /int16_t easeAutonomousGaze\(int16_t current, int16_t target\)/);
});
