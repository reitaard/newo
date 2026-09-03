import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("Autonomy V2 keeps episode ownership while gaze and blink mechanics stay independent", async () => {
  const [header, display, blinkSource, gazeHeader, gazeSource] = await Promise.all([
    source("../../Newo/newo_display.h"),
    source("../../Newo/newo_display.cpp"),
    source("../../Newo/newo_display_blink.cpp"),
    source("../../Newo/newo_gaze_motion.h"),
    source("../../Newo/newo_gaze_motion.cpp"),
  ]);

  assert.match(header, /enum class AutonomousEpisode[\s\S]*WAITING[\s\S]*CURIOUS_SCAN[\s\S]*LOW_ENERGY[\s\S]*SOCIAL_ATTENTION[\s\S]*ALERT_CHECK[\s\S]*DROWSY_REST/);
  assert.match(display, /bool NewoDisplay::autonomousIdle\(\) const \{\s*return effectiveMode\(millis\(\)\) == NewoDisplayMode::IDLE && autoFaceEnabled_;/);
  assert.match(display, /void NewoDisplay::resetFaceMotion\(uint32_t now\)[\s\S]*resetAutonomousEpisode\(now\);/);
  assert.match(display, /constexpr uint32_t kAutonomousEpisodeMinMs = 8'000;[\s\S]*constexpr uint32_t kAutonomousEpisodeMaxMs = 18'000;/);
  assert.match(display, /void NewoDisplay::updateAutonomousEpisode\(uint32_t now\)[\s\S]*chooseAutonomousEpisode\(now\)/);
  assert.match(display, /void NewoDisplay::chooseAutonomousGazeTarget\(\)[\s\S]*random\(12, sideRangeX \+ 1\)/);

  // Behaviors request blinks, but Phase B's dedicated service owns when and
  // how the existing blink state machine runs.
  assert.match(header, /void updateBlinkBeforeFrame\(uint32_t now, NewoDisplayMode activeMode\)/);
  assert.match(blinkSource, /AutonomousEpisode::LOW_ENERGY/);
  assert.match(blinkSource, /AutonomousEpisode::DROWSY_REST/);
  assert.match(blinkSource, /startBilateralBlink\(false, kForceLongBlink\)/);
  assert.doesNotMatch(display.match(/void NewoDisplay::drawFaceFrame\(uint32_t now\) \{([\s\S]*?)\n\}/)?.[1] ?? "",
                      /startBilateralBlink|blinkSchedulerState_|nextBlinkMs_/);

  assert.match(header, /NewoGazeMotion gazeMotion_/);
  assert.match(gazeHeader, /class NewoGazeMotion/);
  assert.match(gazeHeader, /ANTICIPATE, TRAVEL, SETTLE/);
  assert.match(gazeSource, /currentX - directionX_ \* 2/);
  assert.match(gazeSource, /destinationX_ \+ directionX_ \* 2/);
  assert.match(gazeSource, /clamp\(static_cast<int16_t>/);
  assert.doesNotMatch(gazeSource, /malloc|new\s|std::vector|std::map/);
});
