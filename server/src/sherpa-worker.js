import { parentPort, workerData } from "node:worker_threads";
import { resolveSherpaEndpointConfig } from "./sherpa-endpoint-config.js";
import { SherpaAsrBackend } from "./voice.js";

let backend;
const streams = new Map();
let nextSessionId = 1;

function respond(requestId, payload = {}) { parentPort.postMessage({ requestId, ...payload }); }

const endpointConfig = resolveSherpaEndpointConfig(process.env, {
  rule1Seconds: workerData.endpointRule1MinTrailingSilence ?? 2.0,
  rule2Seconds: workerData.endpointRule2MinTrailingSilence ?? 1.0,
  rule3Seconds: workerData.endpointRule3MinUtteranceLength ?? 20,
});
const asrOptions = {
  ...workerData,
  endpointRule1MinTrailingSilence: endpointConfig.rule1Seconds,
  endpointRule2MinTrailingSilence: endpointConfig.rule2Seconds,
  endpointRule3MinUtteranceLength: endpointConfig.rule3Seconds,
};

try {
  // Native module loading, recognizer ownership, Float32 conversion, and every
  // synchronous decode occur in this worker, never in Fastify's main thread.
  backend = new SherpaAsrBackend(asrOptions);
  await backend.prewarm();
  parentPort.postMessage({
    type: "ready",
    lmRequested: Boolean(asrOptions.lm?.requested), lmActive: Boolean(asrOptions.lm?.active),
    lmType: asrOptions.lm?.type ?? null, lmPath: asrOptions.lm?.path ?? null,
    lmScale: asrOptions.lm?.scale ?? null, lmReason: asrOptions.lm?.reason ?? null,
    maxActivePaths: asrOptions.maxActivePaths ?? 4, rssBytes: process.memoryUsage().rss, cpuUsage: process.cpuUsage(),
    endpointRule1MinTrailingSilence: asrOptions.endpointRule1MinTrailingSilence,
    endpointRule2MinTrailingSilence: asrOptions.endpointRule2MinTrailingSilence,
    endpointRule3MinUtteranceLength: asrOptions.endpointRule3MinUtteranceLength,
  });
} catch (error) {
  if (asrOptions.lm?.active) {
    try {
      const fallbackLm = { ...asrOptions.lm, active: false, reason: "load_failed" };
      backend = new SherpaAsrBackend({ ...asrOptions, lm: fallbackLm });
      await backend.prewarm();
      parentPort.postMessage({ type: "ready", lmRequested: true, lmActive: false, lmType: fallbackLm.type,
        lmPath: fallbackLm.path, lmScale: fallbackLm.scale, lmReason: fallbackLm.reason,
        maxActivePaths: asrOptions.maxActivePaths ?? 4, rssBytes: process.memoryUsage().rss, cpuUsage: process.cpuUsage(),
        endpointRule1MinTrailingSilence: asrOptions.endpointRule1MinTrailingSilence,
        endpointRule2MinTrailingSilence: asrOptions.endpointRule2MinTrailingSilence,
        endpointRule3MinUtteranceLength: asrOptions.endpointRule3MinUtteranceLength });
    } catch (fallbackError) { parentPort.postMessage({ type: "fatal", error: fallbackError?.message ?? "ASR worker startup failed" }); }
  } else parentPort.postMessage({ type: "fatal", error: error?.message ?? "ASR worker startup failed" });
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
