import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { PassThrough } from "node:stream";
import test from "node:test";

import { LazyFallbackAsrBackend, PythonSherpaAsrBackend } from "../src/python-sherpa-asr.js";

class FakePython extends EventEmitter {
  constructor({ fatal } = {}) {
    super();
    this.stdout = new PassThrough();
    this.stderr = new PassThrough();
    this.killed = false;
    this.exitCode = null;
    this.signalCode = null;
    this.stdin = new PassThrough();
    let input = "";
    this.stdin.on("data", (chunk) => {
      input += chunk.toString();
      while (input.includes("\n")) {
        const at = input.indexOf("\n");
        const message = JSON.parse(input.slice(0, at)); input = input.slice(at + 1);
        if (message.type === "init") this.reply(fatal ? { type: "fatal", error: fatal } : { type: "ready", max_active_paths: 8, rss_bytes: 123, cpu_seconds: 0.2 });
        else if (message.type === "create") this.reply({ request_id: message.request_id, session_id: 7, decode_ms: 0.1 });
        else if (message.type === "audio") {
          this.reply({ type: "event", session_id: 7, event: { type: "partial", stage: "partial", text: "hello" } });
          this.reply({ request_id: message.request_id, decode_ms: 2.5, rss_bytes: 456, cpu_seconds: 0.4 });
        } else if (message.type === "stop") {
          this.reply({ type: "event", session_id: 7, event: { type: "final", stage: "first_pass_final", text: "hello Newo" } });
          this.reply({ request_id: message.request_id, decode_ms: 1.5 });
        }
      }
    });
  }
  reply(message) { setImmediate(() => this.stdout.write(`${JSON.stringify(message)}\n`)); }
  kill() { this.killed = true; this.exitCode = 0; this.emit("exit", 0, null); }
}

test("Python Sherpa proxy preserves streaming partial and final semantics", async () => {
  const child = new FakePython();
  const logs = [];
  const backend = new PythonSherpaAsrBackend({ pythonExecutable: "python", workerScript: "worker.py", maxActivePaths: 8 }, {
    spawnProcess: () => child, logger: { info: (fields) => logs.push(fields), warn() {}, error() {} },
  });
  await backend.prewarm();
  const events = [];
  const stream = await backend.createStream({ format: { sampleRate: 16000, channels: 1, bitsPerSample: 16 }, onEvent: (event) => events.push(event) });
  await stream.acceptAudio(Buffer.alloc(3200));
  await stream.stop();
  assert.deepEqual(events.map(({ type, text }) => [type, text]), [["partial", "hello"], ["final", "hello Newo"]]);
  assert.ok(logs.some(({ event, asr_backend }) => event === "SHERPA_READY" && asr_backend === "sherpa-python"));
  assert.ok(logs.some(({ event, asr_decode_ms }) => event === "SHERPA_PYTHON_DECODE" && asr_decode_ms === 2.5));
  await backend.close();
  assert.equal(child.killed, true);
});

test("Python startup failure closes it before lazily constructing Node fallback", async () => {
  const order = [];
  const primary = { async prewarm() { order.push("python-start"); throw new Error("missing sherpa_onnx"); }, async close() { order.push("python-close"); } };
  const fallback = { async prewarm() { order.push("node-start"); }, async createStream() { return "node-stream"; }, async close() {} };
  const backend = new LazyFallbackAsrBackend(primary, () => { order.push("node-construct"); return fallback; }, { logger: { error() {} } });
  await backend.prewarm();
  assert.deepEqual(order, ["python-start", "python-close", "node-construct", "node-start"]);
  assert.equal(backend.fallbackActive, true);
  assert.deepEqual(backend.getStatus(), { configured: "sherpa-python", effective: "sherpa", fallback_active: true });
  assert.equal(await backend.createStream({}), "node-stream");
});

test("Python proxy rejects incompatible PCM before starting a worker", async () => {
  let spawned = false;
  const backend = new PythonSherpaAsrBackend({}, { spawnProcess: () => { spawned = true; return new FakePython(); } });
  await assert.rejects(backend.createStream({ format: { sampleRate: 8000, channels: 1, bitsPerSample: 16 }, onEvent() {} }), /mono 16 kHz/);
  assert.equal(spawned, false);
});

test("Python proxy closes promptly after the worker already exited", async () => {
  const child = new FakePython();
  child.exitCode = 134;
  const backend = new PythonSherpaAsrBackend({}, { spawnProcess: () => child, logger: { info() {}, warn() {}, error() {} } });
  backend.child = child;
  const started = Date.now();
  await backend.close();
  assert.ok(Date.now() - started < 100, "close should not wait for an exit event that already happened");
  assert.equal(child.killed, false);
});
