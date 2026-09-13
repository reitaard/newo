import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const read = (relative) => readFile(new URL(relative, import.meta.url), "utf8");

test("voice capture is independent from blocking WebSocket/TLS and DSP work", async () => {
  const [config, audio, ring] = await Promise.all([
    read("../../Newo/newo_config.h"),
    read("../../Newo/newo_audio.cpp"),
    read("../../Newo/newo_pcm_ring.h"),
  ]);

  assert.match(config, /VOICE_CAPTURE_BUFFER_MS\s*=\s*10'000/);
  assert.match(config, /VOICE_CAPTURE_BUFFER_BYTES/);
  assert.match(config, /VOICE_TX_MAX_BATCH_FRAMES\s*=\s*5/);
  assert.match(audio, /xTaskCreatePinnedToCoreWithCaps\([\s\S]*voiceCaptureTaskEntry/);
  assert.match(audio, /MALLOC_CAP_SPIRAM/);
  assert.match(audio, /NewoPcmFrameRing/);
  assert.match(audio, /VOICE_PREROLL/);
  assert.match(audio, /overwritten_frames=/);
  assert.match(audio, /VOICE_CAPTURE_OVERWRITE/);
  assert.match(audio, /VOICE_TX_STALL/);
  assert.match(ring, /discards the oldest frame/);

  const captureStart = audio.indexOf("void voiceCaptureTaskEntry");
  const captureEnd = audio.indexOf("}  // namespace", captureStart);
  const streamStart = audio.indexOf("void NewoAudio::streamTask()");
  const streamEnd = audio.indexOf("void NewoAudio::finishStreaming", streamStart);
  assert.ok(captureStart >= 0 && captureEnd > captureStart && streamStart > captureEnd && streamEnd > streamStart);

  const captureSection = audio.slice(captureStart, captureEnd);
  const networkSection = audio.slice(streamStart, streamEnd);
  assert.match(captureSection, /i2s->readBytes/,
    "dedicated capture producer must own I2S reads");
  assert.equal(captureSection.includes("voiceWebSocket_"), false,
    "capture producer must never perform WebSocket work");
  assert.equal(captureSection.includes("webrtc_"), false,
    "capture producer must not wait for DSP setup or processing");
  assert.match(networkSection, /voiceWebSocket_\.loop\(\)/,
    "network consumer must service the WebSocket");
  assert.equal(networkSection.includes("i2s_.readBytes"), false,
    "blocking WebSocket consumer must never directly capture I2S");
  assert.match(networkSection, /ring\.pop\(/,
    "network consumer must drain the producer ring oldest-first");
  assert.match(networkSection, /webrtc_process\(/,
    "DSP must run after preserved PCM is dequeued, not in the capture producer");
});

test("streaming microphone cleanup requires WebRTC NS medium with AGC bypassed", async () => {
  const [config, audio] = await Promise.all([
    read("../../Newo/newo_config.h"),
    read("../../Newo/newo_audio.cpp"),
  ]);

  assert.match(config, /VOICE_WEBRTC_NS_ENABLED\s*=\s*true/);
  assert.match(config, /VOICE_WEBRTC_NS_MODE\s*=\s*1/);
  assert.match(config, /VOICE_WEBRTC_AGC_ENABLED\s*=\s*false/);
  assert.match(audio, /#include "esp_sr_webrtc\.h"/);
  assert.equal(audio.includes('__has_include("esp_sr_webrtc.h")'), false,
    "enabled production NS must not silently compile into a raw-audio fallback");
  assert.match(audio, /webrtc_create\(/);
  assert.match(audio, /webrtc_process\(/);
  assert.match(audio, /webrtc_destroy\(/);
  assert.match(audio, /VOICE_NS_READY/);
  assert.match(audio, /VOICE_NS_FAILED/);
  assert.match(audio, /tx_peak=/);
  assert.match(audio, /tx_rms=/);

  const wakeStart = audio.indexOf("bool NewoAudio::startWakeNet()");
  const streamStart = audio.indexOf("void NewoAudio::streamTask()", wakeStart);
  const wakeSection = audio.slice(wakeStart, streamStart);
  assert.equal(wakeSection.includes("webrtc_process"), false,
    "WakeNet path must remain untouched by streaming-only NS");
});

test("Sherpa endpoint tuning has one validated source and truthful ready telemetry", async () => {
  const [endpointConfig, voice, worker, envExample] = await Promise.all([
    read("../src/sherpa-endpoint-config.js"),
    read("../src/voice.js"),
    read("../src/sherpa-worker.js"),
    read("../.env.example"),
  ]);

  assert.match(endpointConfig, /rule1Seconds:\s*2\.0/);
  assert.match(endpointConfig, /rule2Seconds:\s*1\.0/);
  assert.match(endpointConfig, /rule3Seconds:\s*20/);
  assert.match(endpointConfig, /VOICE_ASR_ENDPOINT_RULE1_S/);
  assert.match(endpointConfig, /VOICE_ASR_ENDPOINT_RULE2_S/);
  assert.match(endpointConfig, /VOICE_ASR_ENDPOINT_RULE3_S/);
  assert.match(endpointConfig, /throw new Error/);

  assert.match(worker, /resolveSherpaEndpointConfig/);
  assert.match(worker, /workerData\.endpointRule1MinTrailingSilence/);
  assert.match(worker, /endpointRule1MinTrailingSilence:\s*endpointConfig\.rule1Seconds/);
  assert.match(worker, /type:\s*"ready"[\s\S]*endpointRule1MinTrailingSilence/);
  assert.match(voice, /message\.endpointRule1MinTrailingSilence/);
  assert.match(voice, /message\.endpointRule2MinTrailingSilence/);
  assert.match(voice, /message\.endpointRule3MinUtteranceLength/);

  assert.match(envExample, /VOICE_ASR_ENDPOINT_RULE1_S=2\.0/);
  assert.match(envExample, /VOICE_ASR_ENDPOINT_RULE2_S=1\.0/);
  assert.match(envExample, /VOICE_ASR_ENDPOINT_RULE3_S=20/);
});
