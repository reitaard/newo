import assert from "node:assert/strict";
import test from "node:test";
import { readFile } from "node:fs/promises";
import { ASSISTANT_SYSTEM_PROMPT, createAssistantRuntime } from "../src/assistant.js";
import { createAssistantTurnRuntime } from "../src/assistant-turn.js";

const turn = { deviceId: "newo-01", streamId: "stream-1", text: "hello Newo" };
const quietLogger = { info() {}, warn() {} };

function jsonResponse(payload, status = 200) {
  return new Response(JSON.stringify(payload), { status, headers: { "content-type": "application/json" } });
}

test("assistant sends one bounded OpenAI-compatible quick-chat request", async () => {
  let request;
  const runtime = createAssistantRuntime({
    enabled: true, baseUrl: "http://127.0.0.1:8181", model: "helix-qwen3-0.6b", logger: quietLogger,
    fetchImpl: async (_url, options) => { request = JSON.parse(options.body); return jsonResponse({ choices: [{ message: { content: "Hello. I am Newo." } }] }); },
  });
  const result = await runtime.respond(turn);
  assert.equal(result.kind, "response");
  assert.equal(result.text, "Hello. I am Newo.");
  assert.equal(request.model, "helix-qwen3-0.6b");
  assert.equal(request.messages.at(-1).content, turn.text);
  assert.equal(request.reasoning_effort, "none");
});

test("assistant identifies Newo as pronounced Neo", async () => {
  let request;
  const runtime = createAssistantRuntime({
    enabled: true, baseUrl: "http://local", model: "model", logger: quietLogger,
    fetchImpl: async (_url, options) => { request = JSON.parse(options.body); return jsonResponse({ choices: [{ message: { content: "My name is Neo." } }] }); },
  });
  assert.equal((await runtime.respond({ ...turn, text: "What is your name?" })).text, "My name is Neo.");
  assert.match(ASSISTANT_SYSTEM_PROMPT, /Newo, pronounced Neo/);
  assert.match(request.messages[0].content, /refer to your name naturally as Neo/);
});

test("production hotwords only bias the spoken name Neo", async () => {
  const hotwords = await readFile(new URL("../config/newo-hotwords.txt", import.meta.url), "utf8");
  assert.equal(hotwords, "NEO\n");
  assert.doesNotMatch(hotwords, /\bNEWO\b|\bHELLO\b|\bCHECK\b|ONE TWO THREE/);
});

test("assistant timeout and malformed or empty responses settle cleanly", async () => {
  const timeout = createAssistantRuntime({
    enabled: true, baseUrl: "http://local", model: "model", timeoutMs: 10, logger: quietLogger,
    fetchImpl: (_url, { signal }) => new Promise((_resolve, reject) => signal.addEventListener("abort", () => reject(signal.reason))),
  });
  assert.equal((await timeout.respond(turn)).kind, "timeout");
  for (const payload of [{}, { choices: [{ message: { content: "   " } }] }]) {
    const runtime = createAssistantRuntime({ enabled: true, baseUrl: "http://local", model: "model", logger: quietLogger, fetchImpl: async () => jsonResponse(payload) });
    assert.equal((await runtime.respond(turn)).kind, "empty");
  }
  const malformed = createAssistantRuntime({ enabled: true, baseUrl: "http://local", model: "model", logger: quietLogger, fetchImpl: async () => new Response("not json") });
  assert.equal((await malformed.respond(turn)).kind, "error");
});

test("assistant disabled and overlapping device turns never create speaker work", async () => {
  const disabled = createAssistantRuntime({ enabled: false });
  assert.equal((await disabled.respond(turn)).kind, "disabled");
  let disabledSpeakerCalls = 0;
  const disabledTurns = createAssistantTurnRuntime({
    assistant: disabled,
    speakerRuntime: { speak() { disabledSpeakerCalls += 1; return { kind: "queued" }; } },
    isPersistentSpeakerEnabled: () => true, maxReplyChars: 240, logger: quietLogger,
  });
  const disabledFinal = disabledTurns.handleFinalTranscript(turn);
  assert.equal((await disabledFinal.completion).kind, "disabled");
  assert.equal(disabledSpeakerCalls, 0);
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const assistant = createAssistantRuntime({
    enabled: true, baseUrl: "http://local", model: "model", logger: quietLogger,
    fetchImpl: async () => { await gate; return jsonResponse({ choices: [{ message: { content: "ready" } }] }); },
  });
  const spoken = [];
  const turns = createAssistantTurnRuntime({ assistant, speakerRuntime: { speak(text) { spoken.push(text); return { kind: "queued", playbackId: "p", completion: Promise.resolve() }; } }, isPersistentSpeakerEnabled: () => true, maxReplyChars: 240, logger: quietLogger });
  const first = turns.handleFinalTranscript(turn);
  assert.equal(first.kind, "started");
  assert.equal(turns.handleFinalTranscript({ ...turn, streamId: "duplicate" }).kind, "busy");
  release();
  await first.completion;
  assert.deepEqual(spoken, ["ready"]);
});

test("assistant telemetry retains only the latest turn and exposes timeout state", async () => {
  const outcomes = [
    { kind: "response", text: "Ready.", timings: { llm_request_ms: 17 } },
    { kind: "timeout" },
  ];
  const assistant = {
    async respond() { return outcomes.shift(); }, abortDevice() {}, close() {},
    getTelemetry() { return { enabled: true, model: "helix-qwen3-0.6b", qwen: "online", active: false }; },
  };
  const speakerRuntime = { speak() { return { kind: "queued", playbackId: "p", completion: Promise.resolve() }; } };
  const turns = createAssistantTurnRuntime({ assistant, speakerRuntime, isPersistentSpeakerEnabled: () => true, maxReplyChars: 240, logger: quietLogger });
  assert.deepEqual(turns.getTelemetry().latest, { result: "n/a", llmMs: null, streamId: null, at: null, ttsQueuedMs: null, totalMs: null, asrFinalMs: null });
  await turns.handleFinalTranscript({ ...turn, streamId: "first", asrFinalMs: 321 }).completion;
  assert.deepEqual(turns.getTelemetry().latest.result, "complete");
  assert.equal(turns.getTelemetry().latest.llmMs, 17);
  assert.equal(turns.getTelemetry().latest.asrFinalMs, 321);
  await turns.handleFinalTranscript({ ...turn, streamId: "second" }).completion;
  const telemetry = turns.getTelemetry();
  assert.equal(telemetry.status, "error");
  assert.equal(telemetry.latest.result, "timeout");
  assert.equal(telemetry.latest.streamId, "second");
});

test("a valid answer reaches speaker once and speaker failure settles the turn", async () => {
  const assistant = { async respond() { return { kind: "response", text: "Short reply.", timings: { llm_request_ms: 1 } }; }, abortDevice() {}, close() {} };
  let options;
  const speaker = { speak(text, supplied) { options = supplied; assert.equal(text, "Short reply."); return { kind: "queued", playbackId: "p", completion: Promise.reject(new Error("speaker failed")) }; } };
  const turns = createAssistantTurnRuntime({ assistant, speakerRuntime: speaker, isPersistentSpeakerEnabled: () => false, maxReplyChars: 240, logger: quietLogger });
  const started = turns.handleFinalTranscript(turn);
  assert.equal((await started.completion).kind, "speaker_failed");
  assert.equal(options.temporary, true);
  assert.equal(options.metadata.voice_stream_id, "stream-1");
});
