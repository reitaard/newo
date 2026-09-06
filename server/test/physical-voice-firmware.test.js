import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const repoFile = (path) => readFile(new URL(`../../${path}`, import.meta.url), "utf8");

test("physical trigger uses the direct voice path without changing manual toggle", async () => {
  const [audio, sketch] = await Promise.all([repoFile("Newo/newo_audio.cpp"), repoFile("Newo/Newo.ino")]);
  assert.match(audio, /bool NewoAudio::startPhysicalVoiceTrigger\(\)[\s\S]*?beginStreaming\(false\)/);
  assert.match(sketch, /receiveEvent\(arduinoEvent\)/);
  assert.match(sketch, /startPhysicalVoiceTrigger\(\)/);
  assert.match(sketch, /NewoPhysicalVoice::TriggerGate/);
  assert.match(sketch, /Action::MANUAL_TOGGLE[\s\S]*newoAudio\.manualToggle\(\)/);
});

test("Nano peer-ready bootstrap renews the handshake before one armed external trigger", async () => {
  const [node, nano] = await Promise.all([
    repoFile("Newo/newo_arduino_node.cpp"),
    repoFile("arduino/nano-reset-voice-test/nano-reset-voice-test.ino"),
  ]);
  assert.match(node, /strncmp\(frame \+ 10, "READY"/);
  assert.match(node, /source=peer_ready/);
  assert.match(node, /handshakeGeneration_\.fetch_add\(1\)/);
  assert.match(nano, /version=1 capabilities=reset_trigger,led/);
  assert.match(nano, /resetTrigger\.arm\(resetCause\)/);
  assert.match(nano, /if \(resetTrigger\.take\(\)\)/);
  assert.match(nano, /EVENT name=voice_trigger payload=reset/);
});

test("physical feedback stays in Arduino application code", async () => {
  const [sketch, vcp] = await Promise.all([repoFile("Newo/Newo.ino"), repoFile("Newo/newo_usb_vcp.cpp")]);
  assert.match(sketch, /request\("led", NewoPhysicalVoice::ledPayload/);
  assert.doesNotMatch(vcp, /voice_trigger|blink_fast|blink_slow|command=led/);
});
