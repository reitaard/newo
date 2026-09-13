import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const firmware = async (file) => (await readFile(new URL(`../../Newo/${file}`, import.meta.url), "utf8")).replace(/\r\n/g, "\n");

test("manual voice control has one direct OFF-to-STREAMING path without a wake count", async () => {
  const [audio, cloud, sketch] = await Promise.all([
    firmware("newo_audio.cpp"), firmware("newo_cloud.cpp"), firmware("Newo.ino"),
  ]);
  assert.match(cloud, /"manual_toggle"/);
  assert.match(sketch, /Action::MANUAL_TOGGLE/);
  const manual = audio.match(/bool NewoAudio::manualToggle\(\)[\s\S]*?\n}\n\nvoid NewoAudio::finishStreaming/);
  assert.ok(manual);
  assert.match(manual[0], /return beginStreaming\(false\);/);
  assert.match(audio, /if \(beginStreaming\(true\)\) \+\+wakeCount_;/);
  assert.doesNotMatch(manual[0], /\+\+wakeCount_/);
  assert.match(audio, /state_ == NewoVoiceState::STREAMING \|\| streamTask_ \|\| playbackSuppressed_/);
});

test("manual microphone emits one bounded PCM health summary per full diagnostic window", async () => {
  const audio = await firmware("newo_audio.cpp");
  assert.match(audio, /constexpr uint32_t kHealthFrames = 25/);
  assert.match(audio, /VOICE_PCM_HEALTH/);
  assert.match(audio, /frames=%lu samples=%lu peak=%lu rms=%lu nonzero=%lu min=%d max=%d channel=%s/);
  assert.match(audio, /AUDIO_I2S_MIC_IS_LEFT \? "left" : "right"/);
});

test("manual sessions settle OFF while hands-free re-arm waits for assistant completion", async () => {
  const audio = await firmware("newo_audio.cpp");
  assert.match(audio, /strcmp\(reason, "final"\) == 0/);
  assert.match(audio, /awaitingAssistantCompletion_ = true;/);
  assert.match(audio, /void NewoAudio::completeAssistantTurn\(\)[\s\S]*startWakeNet\(\)/);
  assert.match(audio, /!playbackSuppressed_ && !awaitingAssistantCompletion_\) startWakeNet\(\);/);
  assert.match(audio, /rearmAfterStream_ = false;\n  state_ = NewoVoiceState::OFF;/);
  assert.match(audio, /if \(state_ == NewoVoiceState::STREAMING\) \{\n    setEnabled\(false\);/);
  assert.match(audio, /VOICE_MANUAL_BUSY", "speaker_playback/);
  assert.match(audio, /transitionPending_ = false;\n  return true;/);
});

test("failed hands-free capture re-arms locally without waiting for an assistant", async () => {
  const audio = await firmware("newo_audio.cpp");
  const finish = audio.match(/void NewoAudio::finishStreaming[\s\S]*?\n}\n\nvoid NewoAudio::handleVoiceEvent/);
  assert.ok(finish);
  assert.match(finish[0], /successfulHandsFreeFinal/);
  assert.match(finish[0], /if \(enabled_ && !playbackSuppressed_\) startWakeNet\(\);/);
});
