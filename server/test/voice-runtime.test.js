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

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function sendRealtimeFrames(socket, count) {
  for (let index = 0; index < count && socket.readyState === 1; ++index) {
    socket.emit("message", Buffer.alloc(640, index & 0xff), true);
    await sleep(20);
  }
}

async function waitFor(predicate, timeoutMs = 1_000) {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error("timed out waiting for voice test condition");
    await sleep(5);
  }
}

function cadenceRuntime({ latencyForBatch, onAccept, onStop } = {}) {
  const logs = [];
  let batchIndex = 0;
  const runtime = createVoiceRuntime({
    logger: {
      info: (fields, message) => logs.push({ level: "info", fields, message }),
      warn: (fields, message) => logs.push({ level: "warn", fields, message }),
    },
    asr: {
      async createStream() {
        return {
          async acceptAudio(chunk) {
            onAccept?.(chunk);
            const latency = latencyForBatch?.(batchIndex++) ?? 0;
            if (latency) await sleep(latency);
          },
          async stop() { onStop?.(); },
        };
      },
    },
    config: { sampleRate: 16000, channels: 1, bitsPerSample: 16, maxStreamBytes: 640_000,
      batchDurationMs: 100, firstAudioTimeoutMs: 500, saveWav: false, liveTestMode: false },
  });
  return { runtime, logs };
}

test("a synchronous frame burst reserves one active batch before bounding the pending batch", async () => {
  const batches = [];
  const { runtime, logs } = cadenceRuntime({ latencyForBatch: () => 40, onAccept: (chunk) => batches.push(chunk.length) });
  const socket = new FakeSocket();
  await runtime.handleConnection(socket, "test-device");
  for (let index = 0; index < 10; ++index) socket.emit("message", Buffer.alloc(640), true);
  assert.equal(socket.closeCode, undefined);
  socket.close();
  await waitFor(() => logs.some(({ message }) => message === "VOICE_ASR_BATCH"));
  assert.deepEqual(batches, [3200, 3200]);
  const summaries = logs.filter(({ message }) => message === "VOICE_ASR_BATCH");
  assert.equal(summaries.length, 1);
  assert.equal(summaries[0].fields.max_pending_audio_ms, 200);
});

test("realtime 20 ms PCM remains open when 100 ms ASR batches finish within realtime", async () => {
  const batches = [];
  const { runtime, logs } = cadenceRuntime({ latencyForBatch: () => 35, onAccept: (chunk) => batches.push(chunk.length) });
  const socket = new FakeSocket();
  await runtime.handleConnection(socket, "test-device");
  await sendRealtimeFrames(socket, 100); // Two seconds at the physical ESP cadence.
  assert.equal(socket.closeCode, undefined);
  socket.close();
  await waitFor(() => logs.some(({ message }) => message === "VOICE_ASR_BATCH"));
  assert.deepEqual(new Set(batches), new Set([3200]));
  assert.equal(batches.length, 20);
  const summaries = logs.filter(({ message }) => message === "VOICE_ASR_BATCH");
  assert.equal(summaries.length, 1);
  const summary = summaries[0].fields;
  assert.equal(summary.raw_frames_received, 100);
  assert.equal(summary.asr_batches_sent, 20);
  assert.ok(summary.max_pending_audio_ms <= 200);
  assert.equal(summary.worker_backpressure_events, 0);
});

test("one slightly slow worker batch is absorbed by the bounded pending bundle", async () => {
  const { runtime, logs } = cadenceRuntime({ latencyForBatch: (index) => index === 2 ? 105 : 30 });
  const socket = new FakeSocket();
  await runtime.handleConnection(socket, "test-device");
  await sendRealtimeFrames(socket, 60);
  assert.equal(socket.closeCode, undefined);
  socket.close();
  await waitFor(() => logs.some(({ message }) => message === "VOICE_ASR_BATCH"));
  const summaries = logs.filter(({ message }) => message === "VOICE_ASR_BATCH");
  assert.equal(summaries.length, 1);
  const summary = summaries[0].fields;
  assert.equal(summary.asr_batches_sent, 12);
  assert.ok(summary.max_pending_audio_ms <= 200);
  assert.equal(summary.worker_backpressure_events, 0);
});

test("sustained slower-than-realtime ASR closes once with a bounded backlog", async () => {
  const { runtime, logs } = cadenceRuntime({ latencyForBatch: () => 180 });
  const socket = new FakeSocket();
  await runtime.handleConnection(socket, "test-device");
  await sendRealtimeFrames(socket, 60);
  await waitFor(() => logs.some(({ message }) => message === "VOICE_ASR_BATCH"));
  assert.equal(socket.closeCode, 1013);
  assert.equal(logs.filter(({ message }) => message === "Voice worker backpressure limit reached").length, 1);
  const summaries = logs.filter(({ message }) => message === "VOICE_ASR_BATCH");
  assert.equal(summaries.length, 1);
  const summary = summaries[0].fields;
  assert.ok(summary.max_pending_audio_ms <= 200);
  assert.equal(summary.worker_backpressure_events, 1);
  assert.ok(summary.raw_frames_received < 60);
});

test("cleanup waits for active decode and flushes one final partial ASR bundle in chronological order", async () => {
  const batches = [];
  const { runtime, logs } = cadenceRuntime({ latencyForBatch: () => 30, onAccept: (chunk) => batches.push(Buffer.from(chunk)) });
  const socket = new FakeSocket();
  await runtime.handleConnection(socket, "test-device");
  for (let index = 0; index < 5; ++index) socket.emit("message", Buffer.alloc(640, 0x11), true);
  socket.emit("message", Buffer.alloc(640, 0x22), true);
  socket.emit("message", Buffer.alloc(640, 0x33), true);
  socket.close();
  await waitFor(() => logs.some(({ message }) => message === "VOICE_ASR_BATCH"));
  assert.equal(batches.length, 2);
  assert.equal(batches[0].length, 3200);
  assert.ok(batches[0].every((value) => value === 0x11));
  assert.equal(batches[1].length, 1280);
  assert.ok(batches[1].subarray(0, 640).every((value) => value === 0x22));
  assert.ok(batches[1].subarray(640).every((value) => value === 0x33));
  assert.equal(logs.filter(({ message }) => message === "VOICE_ASR_BATCH").length, 1);
});
