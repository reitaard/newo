import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const firmware = async (file) => (await readFile(
  new URL(`../../Newo/${file}`, import.meta.url), "utf8")).replace(/\r\n/g, "\n");

test("hands-free WakeNet stays released through the complete assistant turn", async () => {
  const [audio, cloud, header, sketch] = await Promise.all([
    firmware("newo_audio.cpp"), firmware("newo_cloud.cpp"),
    firmware("newo_cloud.h"), firmware("Newo.ino"),
  ]);
  const finish = audio.match(/void NewoAudio::finishStreaming[\s\S]*?\n}\n\nvoid NewoAudio::handleVoiceEvent/);
  assert.ok(finish);
  assert.doesNotMatch(finish[0], /rearmAfterStream_ && enabled_ && startWakeNet/);
  assert.match(finish[0], /WAKENET_REARM_DEFERRED/);
  assert.match(audio, /!playbackSuppressed_ && !awaitingAssistantCompletion_\) startWakeNet\(\);/);
  assert.match(audio, /if \(awaitingAssistantCompletion_\) \{[\s\S]*progress acknowledgement[\s\S]*assistant_state=idle/);
  assert.doesNotMatch(audio.match(/bool NewoAudio::setPlaybackActive[\s\S]*?\n}\n\nvoid NewoAudio::completeAssistantTurn/)[0], /completeAssistantTurn\(\);/);
  assert.match(audio, /state_ == NewoVoiceState::STREAMING && rearmAfterStream_[\s\S]*assistantTerminalSeen_ = true/);
  assert.match(audio, /rearmAfterStream_ = rearmAfterStream;\n  assistantTerminalSeen_ = false;/);
  assert.match(finish[0], /successfulHandsFreeFinal && !assistantTerminalSeen_/);
  assert.match(cloud, /strcmp\(state, "idle"\) == 0\) assistantTurnTerminalPending_ = true/);
  assert.match(cloud, /case WStype_DISCONNECTED:[\s\S]*assistantTurnTerminalPending_ = true/);
  assert.match(header, /consumeAssistantTurnTerminal/);
  assert.match(sketch, /consumeAssistantTurnTerminal\(\)\) newoAudio\.completeAssistantTurn\(\)/);
});
