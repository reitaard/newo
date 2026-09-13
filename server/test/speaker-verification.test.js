import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { SpeakerVerifier } from "../src/speaker-verification.js";

class FakeWorker extends EventEmitter {
  constructor(embeddings) { super(); this.embeddings = [...embeddings]; setImmediate(() => this.emit("message", { type: "ready", dim: 3 })); }
  postMessage(message) {
    setImmediate(() => this.emit("message", message.type === "finish"
      ? { requestId: message.requestId, ready: true, embedding: new Float32Array(this.embeddings.shift()) }
      : { requestId: message.requestId }));
  }
  async terminate() { return 0; }
}

test("owner enrollment averages three real embeddings and verification reports owner/unknown", async () => {
  const directory = await mkdtemp(path.join(os.tmpdir(), "newo-voiceprint-"));
  const worker = new FakeWorker([[1, 0, 0], [0.9, 0.1, 0], [1, 0, 0], [1, 0, 0], [0, 1, 0]]);
  const verifier = new SpeakerVerifier({ modelPath: "model.onnx", storageDirectory: directory, threshold: 0.8, workerFactory: () => worker });
  verifier.beginEnrollment("newo-01");
  for (let index = 0; index < 3; index += 1) {
    const stream = await verifier.createSession({ deviceId: "newo-01", streamId: `enroll-${index}`, format: { sampleRate: 16000, channels: 1, bitsPerSample: 16 } });
    stream.setTranscript("Hi Wall-E");
    await stream.acceptAudio(Buffer.alloc(3200));
    await stream.finish();
  }
  assert.equal(verifier.status("newo-01").enrolled, true);
  const stored = JSON.parse(await readFile(path.join(directory, "newo-01-owner.json"), "utf8"));
  assert.equal(stored.samples, 3);
  assert.equal(stored.embedding.length, 3);

  const owner = await verifier.createSession({ deviceId: "newo-01", streamId: "owner", format: { sampleRate: 16000, channels: 1, bitsPerSample: 16 } });
  assert.equal((await owner.finish()).identity, "owner");
  const unknown = await verifier.createSession({ deviceId: "newo-01", streamId: "unknown", format: { sampleRate: 16000, channels: 1, bitsPerSample: 16 } });
  assert.equal((await unknown.finish()).identity, "unknown");
  await verifier.close();
  await rm(directory, { recursive: true });
});

test("enrollment rejects the wrong phrase without consuming a sample", async () => {
  const directory = await mkdtemp(path.join(os.tmpdir(), "newo-voiceprint-"));
  const verifier = new SpeakerVerifier({ modelPath: "model.onnx", storageDirectory: directory, workerFactory: () => new FakeWorker([[1, 0, 0]]) });
  verifier.beginEnrollment("newo-01");
  const stream = await verifier.createSession({ deviceId: "newo-01", streamId: "wrong", format: { sampleRate: 16000, channels: 1, bitsPerSample: 16 } });
  stream.setTranscript("hello Newo");
  assert.equal((await stream.finish()).rejected, "phrase_mismatch");
  assert.equal(verifier.status("newo-01").enrollment_samples, 0);
  await verifier.close();
  await rm(directory, { recursive: true });
});
