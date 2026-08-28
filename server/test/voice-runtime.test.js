import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import test from "node:test";
import { createVoiceRuntime } from "../src/voice.js";

class FakeSocket extends EventEmitter {
  constructor() { super(); this.readyState = 1; }
  send() {}
  close() { this.readyState = 3; this.emit("close", 1000); }
}

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
