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
  assert.match(header, /setAssistantThinking\(bool active\)/);

  assert.match(
    display,
    /if \(mode_ == NewoDisplayMode::ERROR[\s\S]*if \(listeningActive_ \|\|[\s\S]*if \(speakerActive_ \|\|[\s\S]*if \(assistantThinking_ \|\|/,
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

  assert.match(cloud, /display_\.setAssistantThinking\(true\)/);
  assert.match(cloud, /display_\.setAssistantThinking\(false\)/);
});
