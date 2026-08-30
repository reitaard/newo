import assert from "node:assert/strict";
import { createServer } from "node:http";
import test from "node:test";

import {
  analyzePcm16,
  createSpeakerRuntime,
  KokoroTtsBackend,
  speakerAudioFilter,
  speakerCreditBytes,
  speakerOutstandingBytes,
  SPEAKER_AUDIO_FILTER,
  SPEAKER_GAIN_DB,
  SPEAKER_INITIAL_LEAD_BYTES,
  SPEAKER_LIMITER,
  SPEAKER_MAX_OUTSTANDING_BYTES,
  SPEAKER_TARGET_OUTSTANDING_BYTES,
  telegramHtmlToSpeech,
} from "../src/tts.js";

const tick = () => new Promise((resolve) => setImmediate(resolve));
async function waitFor(predicate, attempts = 200) {
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
    emitMessage(value) { listeners.get("message")?.(Buffer.from(JSON.stringify(value)), false); },
    close() { this.closeCount += 1; this.readyState = 3; listeners.get("close")?.(); },
  };
}

function logger() { return { info() {}, warn() {} }; }
function binaryFrames(ws) { return ws.frames.filter(Buffer.isBuffer); }

test("Telegram HTML becomes bounded natural speech", () => {
  assert.equal(
    telegramHtmlToSpeech('<b><i>ping:</i></b>\n<blockquote>Status: <b>Online</b>\nLatency: <b>42</b> <i>ms</i></blockquote>'),
    "Status: Online. Latency: 42 milliseconds.",
  );
  assert.equal(telegramHtmlToSpeech("<b>A &amp; B</b> &lt;ready&gt;"), "A & B <ready>");
  assert.ok(telegramHtmlToSpeech("word ".repeat(100), 80).length <= 81);
});

test("speaker flow window preserves 384 ms target and 448 ms ceiling at 24 kHz", () => {
  assert.equal(SPEAKER_INITIAL_LEAD_BYTES, 18_432);
  assert.equal(SPEAKER_TARGET_OUTSTANDING_BYTES, 18_432);
  assert.equal(SPEAKER_MAX_OUTSTANDING_BYTES, 21_504);
  assert.equal(speakerOutstandingBytes(18_432, 0), 18_432);
  assert.equal(speakerCreditBytes(18_432, 0), 0);
  assert.equal(speakerCreditBytes(18_432, 1_024), 1_024);
  assert.equal(speakerCreditBytes(21_504, 1_024), 0);
  assert.throws(() => speakerOutstandingBytes(1_024, 2_048), /invalid speaker flow counters/);
});

test("speaker conditioning adds configurable gain before the limiter", () => {
  assert.equal(SPEAKER_GAIN_DB, 2);
  assert.equal(SPEAKER_LIMITER, 0.95);
  assert.match(SPEAKER_AUDIO_FILTER, /highpass=f=110,volume=2dB:precision=float,alimiter=limit=0\.95/);
  assert.match(SPEAKER_AUDIO_FILTER, /attack=5/);
  assert.match(SPEAKER_AUDIO_FILTER, /level=false/);
  assert.match(speakerAudioFilter(3), /volume=3dB/);
});

test("Kokoro backend requests bounded raw PCM with a configurable voice", async () => {
  let requestBody;
  const server = createServer((request, response) => {
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => {
      requestBody = JSON.parse(Buffer.concat(chunks).toString("utf8"));
      response.writeHead(200, { "content-type": "application/octet-stream" });
      response.end(Buffer.from([1, 0, 2, 0]));
    });
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  let conditioning;
  const backend = new KokoroTtsBackend({
    baseUrl: `http://127.0.0.1:${port}`,
    voice: "bm_george",
    conditioner: async (pcm, format, options) => { conditioning = { pcm, format, options }; return pcm; },
  });
  try {
    const pcm = await backend.synthesize("Natural speech.", { sampleRate: 24_000, channels: 1, bitsPerSample: 16 });
    assert.deepEqual(pcm, Buffer.from([1, 0, 2, 0]));
    assert.deepEqual(requestBody, { model: "kokoro", input: "Natural speech.", voice: "bm_george", response_format: "pcm", speed: 1 });
    assert.equal(conditioning.format.sampleRate, 24_000);
    assert.equal(conditioning.options.gainDb, 2);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

test("Kokoro backend reports an unavailable local service clearly", async () => {
  const backend = new KokoroTtsBackend({ baseUrl: "http://127.0.0.1:1", requestTimeoutMs: 1_000 });
  await assert.rejects(
    backend.synthesize("test", { sampleRate: 24_000, channels: 1, bitsPerSample: 16 }),
    /Kokoro unavailable/,
  );
});

test("Kokoro backend rejects compressed or malformed output", async () => {
  for (const fixture of [
    { type: "audio/mpeg", body: Buffer.from("ID3") },
    { type: "application/octet-stream", body: Buffer.from([1]) },
  ]) {
    const server = createServer((request, response) => { response.writeHead(200, { "content-type": fixture.type }); response.end(fixture.body); });
    await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
    const { port } = server.address();
    const backend = new KokoroTtsBackend({ baseUrl: `http://127.0.0.1:${port}`, conditioner: async (pcm) => pcm });
    try {
      await assert.rejects(
        backend.synthesize("test", { sampleRate: 24_000, channels: 1, bitsPerSample: 16 }),
        /non-PCM content type|invalid PCM16/,
      );
    } finally {
      await new Promise((resolve) => server.close(resolve));
    }
  }
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

test("receiver credit stops after initial lead and resumes only after ESP consumption", async () => {
  const ws = fakeSocket();
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(24_576, 1); }, gainDb: 2, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true,
    flowTimeoutMs: 500,
  });
  runtime.handleConnection(ws, "newo-01");
  const queued = runtime.speak("flow controlled");

  await waitFor(() => binaryFrames(ws).length === 18);
  await tick();
  assert.equal(binaryFrames(ws).length, 18, "sender must stop at the 18 KiB initial window");
  assert.equal(ws.frames.some((frame) => String(frame).includes("speaker_end")), false);

  for (let consumed = 1_024; consumed <= 6_144; consumed += 1_024) {
    const before = binaryFrames(ws).length;
    ws.emitMessage({ type: "speaker_flow", playback_id: queued.playbackId, consumed_bytes: consumed, buffered_bytes: 18_432 - consumed });
    await waitFor(() => binaryFrames(ws).length === before + 1);
  }

  await waitFor(() => ws.frames.some((frame) => String(frame).includes("speaker_end")));
  assert.equal(binaryFrames(ws).length, 24);
  runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: queued.playbackId, bytes: 24_576 });
  assert.equal((await queued.completion).kind, "complete");
  runtime.close();
});

test("speaker runtime fails rather than guessing when receiver flow stops", async () => {
  const ws = fakeSocket();
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(24_576, 1); }, gainDb: 2, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true,
    flowTimeoutMs: 20,
  });
  runtime.handleConnection(ws, "newo-01");
  const queued = runtime.speak("no receiver credit");
  await assert.rejects(queued.completion, /speaker flow timeout/);
  assert.equal(binaryFrames(ws).length, 18);
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
  const keepAlive = setTimeout(() => {}, 100);
  await assert.rejects(queued.completion, /speaker connection timeout/);
  clearTimeout(keepAlive);
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
