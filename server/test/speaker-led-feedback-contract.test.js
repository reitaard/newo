import assert from "node:assert/strict";
import test from "node:test";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("reconnect speaker synchronization explicitly suppresses command LED feedback", async () => {
  const index = await source("../src/index.js");
  assert.match(index, /sendDeviceRequest\("speaker_control", "speaker_ack", \{ action: "set_enabled", enabled: automaticSpeakerEnabled, led_feedback: false \}\)/);
});

test("firmware defaults speaker command LED feedback to false and preserves it through deferred ACKs", async () => {
  const [cloudHeader, cloud, sketch] = await Promise.all([
    source("../../Newo/newo_cloud.h"),
    source("../../Newo/newo_cloud.cpp"),
    source("../../Newo/Newo.ino"),
  ]);
  assert.match(cloudHeader, /bool ledFeedback = false;/);
  assert.match(cloud, /request\.ledFeedback = doc\["led_feedback"\]\.is<bool>\(\) && doc\["led_feedback"\]\.as<bool>\(\);/);
  assert.match(sketch, /pending\.ledFeedback = speakerControlRequest\.ledFeedback;/);
  assert.match(sketch, /if \(speakerControlRequest\.ledFeedback\) \{\s*if \(speakerStateConfirmed\) newoLed\.flashSpeakerEnabled\(speakerControlRequest\.enabled\);\s*else if \(!applied\) newoLed\.flashError\(\);\s*\}/s);
  assert.match(sketch, /if \(pending\.ledFeedback\) \{\s*if \(confirmed\) newoLed\.flashSpeakerEnabled\(pending\.targetEnabled\);\s*else newoLed\.flashError\(\);\s*\}/s);
});
