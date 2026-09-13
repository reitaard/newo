import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const root = new URL("../../", import.meta.url);
const source = (path) => readFileSync(new URL(path, root), "utf8");

test("microphone processing is exclusive, persistent, measured, and RAM-instrumented", () => {
  const audio = source("Newo/newo_audio.cpp");
  const storage = source("Newo/newo_storage.cpp");
  const ino = source("Newo/Newo.ino");
  assert.match(audio, /micMode_ == MicMode::NS/);
  assert.match(audio, /BEFORE_WEBRTC_CREATE/);
  assert.match(audio, /AFTER_WEBRTC_CREATE/);
  assert.match(audio, /AFTER_WEBRTC_DESTROY/);
  assert.match(audio, /raw_clipped=/);
  assert.match(audio, /noise_floor_rms=/);
  assert.match(storage, /mic-mode/);
  assert.match(storage, /mic-ns-level/);
  assert.match(ino, /setMicProcessing/);
  assert.doesNotMatch(audio, /VOICE_WEBRTC_AGC_ENABLED\s*\?\s*true/);
});
