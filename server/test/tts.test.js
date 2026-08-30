import assert from "node:assert/strict";
import test from "node:test";

import {
  analyzePcm16,
  createSpeakerRuntime,
  speakerAudioFilter,
  speakerChunkDueMs,
  SPEAKER_AUDIO_FILTER,
  SPEAKER_GAIN_DB,
  SPEAKER_INITIAL_LEAD_BYTES,
  SPEAKER_LIMITER,
  telegramHtmlToSpeech,
} from "../src/tts.js";

const tick = () => new Promise((resolve) => setImmediate(resolve));
async function waitFor(predicate, attempts = 100) {
  for (let index = 0; index < attempts; index += 1) {
    if (predicate()) return;
    await tick();
  }
  throw new Error("condition not reached");
}

function fakeSocket() {
  const listeners = new Map();
  return {
    readyState: 1,
    frames: [],
    closeCount: 0,
    on(name, callback) { listeners.set(name, callback); },
    send(data, options, callback) {
      this.frames.push(data);
      (typeof options === "function" ? options : callback)?.();
    },
    close() { this.closeCount += 1; this.readyState = 3; listeners.get("close")?.(); },
  };
}

function logger() { return { info() {}, warn() {} }; }

test("Telegram HTML becomes bounded natural speech", () => {
  assert.equal(
    telegramHtmlToSpeech('<b><i>ping:</i></b>\n<blockquote>Status: <b>Online</b>\nLatency: <b>42</b> <i>ms</i></blockquote>'),
    "Status: Online. Latency: 42 milliseconds.",
  );
  assert.equal(telegramHtmlToSpeech("<b>A &amp; B</b> &lt;ready&gt;"), "A & B <ready>");
  assert.ok(telegramHtmlToSpeech("word ".repeat(100), 80).length <= 81);
});

test("speaker pacing keeps the 256 ms lead and absolute PCM timeline", () => {
  assert.equal(SPEAKER_INITIAL_LEAD_BYTES, 8_192);
  assert.equal(speakerChunkDueMs(4_096, 8_192, 32_000), 0);
  assert.equal(speakerChunkDueMs(8_192, 8_192, 32_000), 0);
  assert.equal(speakerChunkDueMs(9_216, 8_192, 32_000), 32);
  assert.equal(speakerChunkDueMs(10_240, 8_192, 32_000), 64);
  assert.throws(() => speakerChunkDueMs(1_024, 8_192, 0), /invalid PCM byte rate/);
});

test("speaker conditioning adds configurable gain before the limiter", () => {
  assert.equal(SPEAKER_GAIN_DB, 6);
  assert.equal(SPEAKER_LIMITER, 0.95);
  assert.match(SPEAKER_AUDIO_FILTER, /highpass=f=110,volume=6dB:precision=float,alimiter=limit=0\.95/);
  assert.match(SPEAKER_AUDIO_FILTER, /attack=5/);
  assert.match(SPEAKER_AUDIO_FILTER, /level=false/);
  assert.match(speakerAudioFilter(3), /volume=3dB/);
});

test("final PCM diagnostics report peak, RMS, limiter reach, and clipping", () => {
  const pcm = Buffer.alloc(8);
  pcm.writeInt16LE(0, 0);
  pcm.writeInt16LE(16_384, 2);
  pcm.writeInt16LE(-16_384, 4);
  pcm.writeInt16LE(0, 6);
  const diagnostics = analyzePcm16(pcm, 0.95);
  assert.equal(diagnostics.peakDbfs.toFixed(1), "-6.0");
  assert.equal(diagnostics.rmsDbfs.toFixed(1), "-9.0");
  assert.equal(diagnostics.limiterReached, false);
  assert.equal(diagnostics.clipped, false);
  assert.equal(diagnostics.bytes, 8);
});

test("speaker runtime bounds queued jobs", () => {
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { synthesize: () => new Promise(() => {}), gainDb: 6, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true, maxPendingJobs: 2,
  });
  assert.equal(runtime.speak("one").kind, "queued");
  assert.equal(runtime.speak("two").kind, "queued");
  assert.equal(runtime.speak("three").kind, "busy");
  runtime.close();
});

test("one persistent speaker connection is reused for 10 framed playbacks", async () => {
  const ws = fakeSocket();
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(4_096, 1); }, gainDb: 6, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true,
  });
  runtime.handleConnection(ws, "newo-01");

  const first = runtime.speak("first");
  await waitFor(() => ws.frames.some((frame) => String(frame).includes("speaker_end")));
  const firstFrames = [...ws.frames];
  assert.match(String(firstFrames[0]), /"type":"speaker_begin"/);
  assert.deepEqual(firstFrames.filter(Buffer.isBuffer).map((frame) => frame.length), [1_024, 1_024, 1_024, 1_024]);
  assert.match(String(firstFrames.at(-1)), /"type":"speaker_end"/);
  assert.equal(ws.closeCount, 0);
  runtime.handlePlaybackStarted("newo-01", { playback_id: first.playbackId, first_pcm_to_play_ms: 128 });
  runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: first.playbackId, bytes: 4_096 });
  assert.equal((await first.completion).kind, "complete");

  for (let number = 2; number <= 10; number += 1) {
    const frameCount = ws.frames.length;
    const queued = runtime.speak(`playback ${number}`);
    await waitFor(() => ws.frames.length > frameCount && String(ws.frames.at(-1)).includes("speaker_end"));
    runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: queued.playbackId, bytes: 4_096 });
    await queued.completion;
  }
  assert.equal(ws.closeCount, 0);
  assert.equal(ws.frames.filter((frame) => String(frame).includes("speaker_begin")).length, 10);
  runtime.close();
});

test("speaker runtime fails quickly when no persistent stream becomes ready", async () => {
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(512); }, gainDb: 6, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true,
    connectionTimeoutMs: 20,
  });
  const queued = runtime.speak("hello");
  await assert.rejects(queued.completion, /speaker connection timeout/);
  runtime.close();
});

test("manual speech requests a temporary stream and disconnects it after completion", async () => {
  let persistentEnabled = false;
  const controls = [];
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(512); }, gainDb: 6, limiter: 0.95 },
    getDevice: () => ({}),
    sendControl: (message, device) => { controls.push({ message, device }); return true; },
    isPersistentEnabled: () => persistentEnabled,
  });
  const queued = runtime.speak("manual", { temporary: true });
  await waitFor(() => controls.length === 1);
  assert.deepEqual(controls[0].message, { type: "speaker_control", action: "temporary_connect" });
  const ws = fakeSocket();
  runtime.handleConnection(ws, "newo-01");
  await waitFor(() => String(ws.frames.at(-1)).includes("speaker_end"));
  runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: queued.playbackId, bytes: 512 });
  await queued.completion;
  assert.equal(ws.closeCount, 1);
  assert.equal(persistentEnabled, false);
  runtime.close();
});
