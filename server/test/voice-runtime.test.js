import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import test from "node:test";
import { createVoiceRuntime, WorkerAsrBackend } from "../src/voice.js";

class FakeSocket extends EventEmitter {
  constructor() { super(); this.readyState = 1; }
  send() {}
  close(code = 1000) { this.closeCode = code; this.readyState = 3; this.emit("close", code); }
}

class FakeSherpaWorker extends EventEmitter {
  constructor({ fatal = null } = {}) {
    super();
    this.terminated = false;
    setImmediate(() => this.emit("message", fatal ? { type: "fatal", error: fatal } : { type: "ready" }));
  }
  postMessage(message) {
    setImmediate(() => {
      if (message.type === "create") this.emit("message", { requestId: message.requestId, sessionId: 1 });
      else this.emit("message", { requestId: message.requestId });
    });
  }
  async terminate() { this.terminated = true; this.emit("exit", 0); return 0; }
}

test("Sherpa prewarms to ready and stays warm after its last stream closes", async () => {
  const logs = [];
  const worker = new FakeSherpaWorker();
  const backend = new WorkerAsrBackend({}, {
    workerFactory: () => worker,
    logger: { info: (fields, message) => logs.push({ fields, message }), error() {} },
  });
  await backend.prewarm();
  assert.ok(logs.some(({ message }) => message === "SHERPA_WORKER_STARTING"));
  assert.ok(logs.some(({ message }) => message === "SHERPA_READY"));
  const stream = await backend.createStream({ format: { sampleRate: 16000, channels: 1, bitsPerSample: 16 }, onEvent() {} });
  await stream.stop();
  assert.equal(backend.streams.size, 0);
  assert.equal(worker.terminated, false);
  assert.equal(backend.worker, worker);
  await backend.close();
  assert.equal(worker.terminated, true);
});

test("Sherpa fatal startup rejects preload without leaving initialization pending", async () => {
  const errors = [];
  const worker = new FakeSherpaWorker({ fatal: "model load failed" });
  const backend = new WorkerAsrBackend({}, {
    workerFactory: () => worker,
    logger: { info() {}, error: (fields) => errors.push(fields) },
  });
  await assert.rejects(backend.prewarm(), /model load failed/);
  await assert.rejects(backend.createStream({ format: { sampleRate: 16000, channels: 1, bitsPerSample: 16 }, onEvent() {} }), /model load failed/);
  assert.equal(backend.startPromise, null);
  assert.equal(backend.requests.size, 0);
  assert.equal(errors[0].event, "SHERPA_START_FAILED");
  await backend.close();
});

test("a voice socket closes its ASR stream exactly once", async () => {
  let created = 0;
  let stopped = 0;
  const messages = [];
  const runtime = createVoiceRuntime({
    logger: { info: (fields, msg) => messages.push(msg), warn() {} },
    asr: { async createStream() { created += 1; return { async acceptAudio() {}, async stop() { stopped += 1; } }; } },
    config: { sampleRate: 16000, channels: 1, bitsPerSample: 16, maxStreamBytes: 64000, saveWav: false, liveTestMode: false },
  });
  const socket = new FakeSocket();
  await runtime.handleConnection(socket, "test-device");
  socket.emit("message", Buffer.alloc(3200), true);
  await new Promise((resolve) => setImmediate(resolve));
  socket.emit("error", new Error("simulated"));
  socket.emit("close", 1006);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(created, 1);
  assert.equal(stopped, 1);
  assert.equal(messages.filter((message) => message === "VOICE_ASR_STREAM_CREATED").length, 1);
  assert.equal(messages.filter((message) => message === "VOICE_ASR_STREAM_CLOSED").length, 1);
});

test("only one final ASR event starts an assistant turn; partials never do", async () => {
  const finals = [];
  let emit;
  const runtime = createVoiceRuntime({
    logger: { info() {}, warn() {} },
    asr: { async createStream({ onEvent }) { emit = onEvent; return { async acceptAudio() {}, async stop() { emit({ type: "final", text: "cleanup duplicate" }); } }; } },
    config: { sampleRate: 16000, channels: 1, bitsPerSample: 16, maxStreamBytes: 64000, saveWav: false, liveTestMode: false,
      onFinalTranscript: (turn) => finals.push(turn) },
  });
  const socket = new FakeSocket();
  await runtime.handleConnection(socket, "test-device");
  socket.emit("message", Buffer.alloc(3200), true);
  await new Promise((resolve) => setImmediate(resolve));
  emit({ type: "partial", text: "hello" });
  emit({ type: "final", text: "hello Newo" });
  emit({ type: "final", text: "cleanup duplicate" });
  await new Promise((resolve) => setImmediate(resolve));
  socket.emit("close", 1000);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(finals.length, 1);
  assert.equal(finals[0].text, "hello Newo");
});

test("a connected voice socket with no PCM closes on the bounded first-audio timeout", async () => {
  let created = 0;
  const warnings = [];
  const runtime = createVoiceRuntime({
    logger: { info() {}, warn: (fields, message) => warnings.push({ fields, message }) },
    asr: { async createStream() { created += 1; return { async acceptAudio() {}, async stop() {} }; } },
    config: { sampleRate: 16000, channels: 1, bitsPerSample: 16, maxStreamBytes: 64000, firstAudioTimeoutMs: 10, saveWav: false, liveTestMode: false },
  });
  const socket = new FakeSocket();
  await runtime.handleConnection(socket, "test-device");
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(socket.closeCode, 1008);
  assert.equal(created, 0);
  assert.ok(warnings.some(({ message }) => message === "voice_no_audio_timeout"));
});

test("first PCM logs arrival and cancels the no-audio timeout", async () => {
  const messages = [];
  const runtime = createVoiceRuntime({
    logger: { info: (fields, message) => messages.push({ fields, message }), warn() {} },
    asr: { async createStream() { return { async acceptAudio() {}, async stop() {} }; } },
    config: { sampleRate: 16000, channels: 1, bitsPerSample: 16, maxStreamBytes: 64000, firstAudioTimeoutMs: 10, saveWav: false, liveTestMode: false },
  });
  const socket = new FakeSocket();
  await runtime.handleConnection(socket, "test-device");
  socket.emit("message", Buffer.alloc(640), true);
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(socket.closeCode, undefined);
  const firstAudio = messages.find(({ message }) => message === "VOICE_FIRST_AUDIO");
  assert.equal(firstAudio.fields.chunk_bytes, 640);
  assert.ok(messages.some(({ message }) => message === "VOICE_ASR_STREAM_CREATED"));
  socket.close();
});

test("voice runtime fails rather than accumulating worker-bound PCM", async () => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const runtime = createVoiceRuntime({
    logger: { info() {}, warn() {} },
    asr: { async createStream() { return { async acceptAudio() { await gate; }, async stop() {} }; } },
    config: { sampleRate: 16000, channels: 1, bitsPerSample: 16, maxStreamBytes: 64000, maxPendingChunks: 2, saveWav: false, liveTestMode: false },
  });
  const socket = new FakeSocket();
  await runtime.handleConnection(socket, "test-device");
  socket.emit("message", Buffer.alloc(640), true);
  socket.emit("message", Buffer.alloc(640), true);
  socket.emit("message", Buffer.alloc(640), true);
  assert.equal(socket.closeCode, 1013);
  release();
});
