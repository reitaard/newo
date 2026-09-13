import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const read = (relative) => readFile(new URL(relative, import.meta.url), "utf8");

test("voice streaming preserves pre-connect PCM in bounded PSRAM", async () => {
  const [config, audio] = await Promise.all([
    read("../../Newo/newo_config.h"),
    read("../../Newo/newo_audio.cpp"),
  ]);

  assert.match(config, /VOICE_PRECONNECT_BUFFER_MS\s*=\s*1'000/);
  assert.match(config, /VOICE_PRECONNECT_BUFFER_BYTES/);
  assert.match(audio, /MALLOC_CAP_SPIRAM/);
  assert.match(audio, /VOICE_PREROLL/);
  assert.match(audio, /overwritten_frames=/);
  assert.match(audio, /buffered_ms=/);

  const capture = audio.indexOf("i2s_.readBytes");
  const disconnectedBuffer = audio.indexOf("if (!voiceConnected_)", capture);
  assert.ok(capture >= 0 && disconnectedBuffer > capture,
    "I2S capture must happen before the disconnected pre-roll branch");
});

test("streaming microphone cleanup uses WebRTC NS medium with AGC bypassed", async () => {
  const [config, audio] = await Promise.all([
    read("../../Newo/newo_config.h"),
    read("../../Newo/newo_audio.cpp"),
  ]);

  assert.match(config, /VOICE_WEBRTC_NS_ENABLED\s*=\s*true/);
  assert.match(config, /VOICE_WEBRTC_NS_MODE\s*=\s*1/);
  assert.match(config, /VOICE_WEBRTC_AGC_ENABLED\s*=\s*false/);
  assert.match(audio, /#include "esp_sr_webrtc\.h"/);
  assert.match(audio, /webrtc_create\(/);
  assert.match(audio, /webrtc_process\(/);
  assert.match(audio, /webrtc_destroy\(/);
  assert.match(audio, /VOICE_NS_READY/);
  assert.match(audio, /VOICE_NS_BYPASS/);
  assert.match(audio, /tx_peak=/);
  assert.match(audio, /tx_rms=/);

  const wakeStart = audio.indexOf("bool NewoAudio::startWakeNet()");
  const streamStart = audio.indexOf("void NewoAudio::streamTask()", wakeStart);
  const wakeSection = audio.slice(wakeStart, streamStart);
  assert.equal(wakeSection.includes("webrtc_process"), false,
    "WakeNet path must remain untouched by streaming-only NS");
});
