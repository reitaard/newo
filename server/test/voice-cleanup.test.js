import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import test from "node:test";

const obsolete = ["AUDIO_QUEUE_DEPTH", "AUDIO_WS_BUNDLE_FRAMES", "AUDIO_WS_BUNDLE_BYTES", "AUDIO_SEND_DRAIN_FRAME_LIMIT", "NewoVoiceHealth"];

test("stale queued-voice transport constants and health subsystem are absent", async () => {
  const config = await readFile(new URL("../../Newo/newo_config.h", import.meta.url), "utf8");
  for (const symbol of obsolete.slice(0, 4)) assert.equal(config.includes(symbol), false, symbol);
  await assert.rejects(access(new URL("../../Newo/newo_voice_health.h", import.meta.url)));
  await assert.rejects(access(new URL("../../Newo/test/voice_health_test.cpp", import.meta.url)));
});
