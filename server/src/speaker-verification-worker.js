import { parentPort, workerData } from "node:worker_threads";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const sherpa = require("sherpa-onnx-node");
const extractor = new sherpa.SpeakerEmbeddingExtractor({
  model: workerData.modelPath,
  numThreads: workerData.numThreads,
  provider: "cpu",
  debug: 0,
});
const streams = new Map();

function reply(requestId, fields = {}) { parentPort.postMessage({ requestId, ...fields }); }

parentPort.on("message", (message) => {
  try {
    if (message.type === "create") {
      streams.set(message.sessionId, extractor.createStream());
      reply(message.requestId);
      return;
    }
    const stream = streams.get(message.sessionId);
    if (!stream) throw new Error("Unknown speaker-verification session");
    if (message.type === "audio") {
      const bytes = Buffer.from(message.chunk);
      const samples = new Float32Array(bytes.length / 2);
      for (let i = 0; i < samples.length; i += 1) samples[i] = bytes.readInt16LE(i * 2) / 32768;
      stream.acceptWaveform({ samples, sampleRate: 16_000 });
      reply(message.requestId);
      return;
    }
    if (message.type === "finish") {
      stream.inputFinished();
      const ready = extractor.isReady(stream);
      const embedding = ready ? extractor.compute(stream) : null;
      streams.delete(message.sessionId);
      reply(message.requestId, { ready, embedding });
      return;
    }
    if (message.type === "cancel") {
      streams.delete(message.sessionId);
      reply(message.requestId);
    }
  } catch (error) {
    if (message.sessionId) streams.delete(message.sessionId);
    reply(message.requestId, { error: error?.message ?? "speaker verification failed" });
  }
});

parentPort.postMessage({ type: "ready", dim: extractor.dim });
