import assert from "node:assert/strict";
import test from "node:test";

import { createSpeakerRuntime, telegramHtmlToSpeech } from "../src/tts.js";

test("Telegram HTML becomes bounded natural speech", () => {
  assert.equal(
    telegramHtmlToSpeech('<b><i>ping:</i></b>\n<blockquote>Status: <b>Online</b>\nLatency: <b>42</b> <i>ms</i></blockquote>'),
    "Status: Online. Latency: 42 milliseconds.",
  );
  assert.equal(telegramHtmlToSpeech("<b>A &amp; B</b> &lt;ready&gt;"), "A & B <ready>");
  assert.equal(telegramHtmlToSpeech("invalid &#99999999; entity"), "invalid &#99999999; entity");
  assert.ok(telegramHtmlToSpeech("word ".repeat(100), 80).length <= 81);
});

test("speaker runtime bounds queued jobs", () => {
  const runtime = createSpeakerRuntime({
    logger: { info() {}, warn() {} }, enabled: true,
    backend: { synthesize: () => new Promise(() => {}) },
    getDevice: () => ({}), sendControl: () => true, maxPendingJobs: 2,
  });
  assert.equal(runtime.speak("one").kind, "queued");
  assert.equal(runtime.speak("two").kind, "queued");
  assert.equal(runtime.speak("three").kind, "busy");
  runtime.close();
});

test("speaker runtime keeps PCM off control websocket and completes from device result", async () => {
  const controls = [];
  const device = {};
  const runtime = createSpeakerRuntime({
    logger: { info() {}, warn() {} }, enabled: true,
    backend: { async synthesize() { return Buffer.alloc(4_096, 1); } },
    getDevice: () => device,
    sendControl: (message) => { controls.push(message); return true; },
    chunkBytes: 2_048,
  });
  const queued = runtime.speak("<b>hello</b>");
  assert.equal(queued.kind, "queued");
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(controls.length, 1);
  assert.equal(controls[0].type, "speaker_play");
  assert.equal(controls[0].bytes, 4_096);
  assert.equal(Object.hasOwn(controls[0], "pcm"), false);

  const frames = [];
  const ws = {
    readyState: 1,
    send(data, options, callback) {
      frames.push(data);
      (typeof options === "function" ? options : callback)?.();
    },
    close() { this.readyState = 3; },
  };
  await runtime.handleConnection(ws, "newo-01", queued.playbackId);
  assert.equal(Buffer.isBuffer(frames[0]), true);
  assert.match(String(frames.at(-1)), /speaker_end/);
  assert.equal(runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: queued.playbackId, bytes: 4_096 }), true);
  assert.equal((await queued.completion).kind, "complete");
});

test("speaker runtime fails quickly when the stream does not connect", async () => {
  const device = {};
  const runtime = createSpeakerRuntime({
    logger: { info() {}, warn() {} }, enabled: true,
    backend: { async synthesize() { return Buffer.alloc(512); } },
    getDevice: () => device,
    sendControl: () => true,
    connectionTimeoutMs: 20,
    resultTimeoutMs: 1_000,
  });
  const queued = runtime.speak("hello");
  await assert.rejects(queued.completion, /speaker connection timeout/);
  runtime.close();
});

test("speaker runtime retries control on a replacement device connection", async () => {
  const oldDevice = {};
  const newDevice = {};
  const controls = [];
  const runtime = createSpeakerRuntime({
    logger: { info() {}, warn() {} }, enabled: true,
    backend: { async synthesize() { return Buffer.alloc(512); } },
    getDevice: () => oldDevice,
    sendControl: (message, device) => { controls.push({ message, device }); return true; },
    connectionTimeoutMs: 1_000,
  });
  const queued = runtime.speak("hello");
  await new Promise((resolve) => setImmediate(resolve));
  await runtime.handleDeviceConnected("newo-01", newDevice);
  assert.equal(controls.length, 2);
  assert.equal(controls[0].device, oldDevice);
  assert.equal(controls[1].device, newDevice);
  assert.equal(controls[1].message.playback_id, queued.playbackId);
  runtime.close();
});
