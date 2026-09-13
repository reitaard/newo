import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("display activity signals are propagated and listening waits for voice connection", async () => {
  const [header, display, audio, cloud, sketch] = await Promise.all([
    source("../../Newo/newo_display.h"),
    source("../../Newo/newo_display.cpp"),
    source("../../Newo/newo_audio.cpp"),
    source("../../Newo/newo_cloud.cpp"),
    source("../../Newo/Newo.ino"),
  ]);

  assert.match(header, /setListeningActive\(bool active\)/);
  assert.match(header, /setAssistantState\(NewoDisplayMode mode\)/);
  assert.match(header, /IDLE, LISTENING, PROCESSING, THINKING, RESPONDING, SPEAKING/);

  assert.match(
    display,
    /if \(mode_ == NewoDisplayMode::ERROR[\s\S]*if \(listeningActive_ \|\|[\s\S]*if \(speakerActive_ \|\|[\s\S]*if \(assistantState_ != NewoDisplayMode::IDLE/,
  );

  assert.doesNotMatch(audio, /display_\.setListeningActive\(true\)/);

  assert.match(
    audio,
    /display_\.setListeningActive\(state_ == NewoVoiceState::STREAMING && voiceConnected_\)/,
  );

  assert.match(audio, /display_\.setListeningActive\(false\)/);

  const gates =
    sketch.match(
      /newoAudio\.state\(\) == NewoVoiceState::STREAMING && newoAudio\.voiceConnected\(\)/g,
    ) ?? [];

  assert.equal(gates.length, 2);

  assert.match(cloud, /"processing"[\s\S]*setAssistantState\(NewoDisplayMode::PROCESSING\)/);
  assert.match(cloud, /"listening"[\s\S]*setAssistantState\(NewoDisplayMode::IDLE\)/);
  assert.match(cloud, /"thinking"[\s\S]*setAssistantState\(NewoDisplayMode::THINKING\)/);
  assert.match(cloud, /"responding"[\s\S]*setAssistantState\(NewoDisplayMode::RESPONDING\)/);
  assert.match(cloud, /"idle"[\s\S]*setAssistantState\(NewoDisplayMode::IDLE\)/);
  assert.match(display, /NewoDisplayMode::PROCESSING[\s\S]*drawFastHLine/);
  assert.match(display, /NewoDisplayMode::RESPONDING[\s\S]*fillCircle/);
});
