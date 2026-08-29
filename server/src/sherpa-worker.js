import { parentPort, workerData } from "node:worker_threads";
import { SherpaAsrBackend } from "./voice.js";

let backend;
const streams = new Map();
let nextSessionId = 1;

function respond(requestId, payload = {}) { parentPort.postMessage({ requestId, ...payload }); }

try {
  // Native module loading, recognizer ownership, Float32 conversion, and every
  // synchronous decode occur in this worker, never in Fastify's main thread.
  backend = new SherpaAsrBackend(workerData);
} catch (error) {
  parentPort.postMessage({ type: "fatal", error: error?.message ?? "ASR worker startup failed" });
}

parentPort.on("message", async (message) => {
  try {
    if (!backend) throw new Error("ASR worker unavailable");
    if (message.type === "create") {
      const sessionId = nextSessionId++;
      const stream = await backend.createStream({
        format: message.format,
        onEvent(event) { parentPort.postMessage({ type: "event", sessionId, event }); },
      });
      streams.set(sessionId, stream);
      respond(message.requestId, { sessionId });
      return;
    }
    const stream = streams.get(message.sessionId);
    if (!stream) throw new Error("ASR session not found");
    if (message.type === "audio") await stream.acceptAudio(Buffer.from(message.chunk));
    else if (message.type === "stop") { await stream.stop(); streams.delete(message.sessionId); }
    else throw new Error("unsupported ASR worker message");
    respond(message.requestId);
  } catch (error) {
    respond(message.requestId, { error: error?.message ?? "ASR worker failure" });
  }
});
