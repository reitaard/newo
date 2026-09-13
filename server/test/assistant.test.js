import assert from "node:assert/strict";
import test from "node:test";
import { readFile } from "node:fs/promises";
import { ASSISTANT_HISTORY_MAX_CHARS_PER_MESSAGE, ASSISTANT_SYSTEM_PROMPT, createAssistantRuntime } from "../src/assistant.js";
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
  assert.equal(request.messages[0].content, ASSISTANT_SYSTEM_PROMPT);
  assert.equal(request.messages[1].role, "system");
  assert.equal(request.temperature, 0.45);
  assert.equal(request.max_tokens, 48);
  assert.equal(Object.hasOwn(request, "reasoning_effort"), false);
  assert.equal(result.timings.history_turns, 0);
});

test("assistant retains only three bounded exchanges per device and clears them with the session", async () => {
  const requests = [];
  let reply = 0;
  const runtime = createAssistantRuntime({
    enabled: true, baseUrl: "http://local", model: "model", logger: quietLogger,
    now: () => new Date("2026-09-12T21:35:12.000Z"),
    fetchImpl: async (_url, options) => {
      requests.push(JSON.parse(options.body));
      reply += 1;
      return jsonResponse({ choices: [{ message: { content: `answer ${reply} ${"x".repeat(300)}` } }] });
    },
  });
  for (let index = 1; index <= 5; index += 1) {
    await runtime.respond({ deviceId: "device-a", streamId: `a-${index}`, text: `question ${index} ${"y".repeat(300)}` });
  }

  const fifth = requests[4];
  const remembered = fifth.messages.slice(2, -1);
  assert.equal(remembered.length, 6);
  assert.doesNotMatch(JSON.stringify(remembered), /question 1|answer 1/);
  assert.match(JSON.stringify(remembered), /question 2/);
  assert.ok(remembered.every((message) => message.content.length <= ASSISTANT_HISTORY_MAX_CHARS_PER_MESSAGE));
  assert.equal(fifth.messages.at(-1).content.startsWith("question 5"), true);

  await runtime.respond({ deviceId: "device-b", streamId: "b-1", text: "what about it?" });
  assert.equal(requests[5].messages.length, 3, "another device must not receive device-a history");
  runtime.abortDevice("device-a");
  await runtime.respond({ deviceId: "device-a", streamId: "a-new", text: "do that again" });
  assert.equal(requests[6].messages.length, 3, "disconnect must clear the device session history");
});

test("assistant includes compact known runtime state and backend usage telemetry", async () => {
  let request;
  const logs = [];
  const runtime = createAssistantRuntime({
    enabled: true, baseUrl: "http://local", model: "model",
    now: () => new Date("2026-09-12T21:35:12.000Z"),
    runtimeContext: () => ({ speakerEnabled: true, speakerVolume: 72, speakerMuted: false,
      cloudStatus: "connected" }),
    logger: { info(fields) { logs.push(fields); }, warn() {} },
    fetchImpl: async (_url, options) => {
      request = JSON.parse(options.body);
      return jsonResponse({ choices: [{ message: { content: "Ready." } }],
        usage: { prompt_tokens: 123, completion_tokens: 4 } });
    },
  });
  const result = await runtime.respond(turn);
  assert.equal(request.messages[2].content,
    "Current runtime state: speaker=enabled, volume=72%, mute=off, cloud=connected.");
  assert.deepEqual(result.timings.input_tokens, 123);
  assert.deepEqual(result.timings.output_tokens, 4);
  assert.equal(result.timings.history_turns, 0);
  assert.ok(result.timings.prompt_chars > turn.text.length);
  assert.equal(logs.at(-1).input_tokens, 123);
  assert.equal(logs.at(-1).output_tokens, 4);
});

test("assistant supplies current server time in the configured IANA timezone per turn", async () => {
  const requests = [];
  const clockValues = [
    new Date("2026-09-12T21:35:12.000Z"),
    new Date("2026-09-12T21:35:13.000Z"),
  ];
  const runtime = createAssistantRuntime({
    enabled: true, baseUrl: "http://local", model: "model", timeZone: "Asia/Phnom_Penh",
    now: () => clockValues.shift(), logger: quietLogger,
    fetchImpl: async (_url, options) => {
      requests.push(JSON.parse(options.body));
      return jsonResponse({ choices: [{ message: { content: "It is four thirty-five AM." } }] });
    },
  });

  await runtime.respond({ ...turn, streamId: "time-1", text: "What time is it?" });
  await runtime.respond({ ...turn, streamId: "time-2", text: "And now?" });

  assert.match(requests[0].messages[1].content,
    /Current local date and time: Sunday, September 13, 2026 at 4:35:12 AM\./);
  assert.match(requests[0].messages[1].content, /Timezone: Asia\/Phnom_Penh \(UTC\+07:00\)\./);
  assert.match(requests[0].messages[1].content, /authoritative for questions about the current date or time/);
  assert.match(requests[1].messages[1].content, /4:35:13 AM/);
  assert.notEqual(requests[0].messages[1].content, requests[1].messages[1].content);
  assert.equal(requests[0].messages[0].content, ASSISTANT_SYSTEM_PROMPT);
});

test("assistant respects another requested timezone and rejects invalid IANA zones", async () => {
  let request;
  const runtime = createAssistantRuntime({
    enabled: true, baseUrl: "http://local", model: "model", timeZone: "America/New_York",
    now: () => new Date("2026-01-15T17:08:09.000Z"), logger: quietLogger,
    fetchImpl: async (_url, options) => {
      request = JSON.parse(options.body);
      return jsonResponse({ choices: [{ message: { content: "It is noon." } }] });
    },
  });
  await runtime.respond(turn);
  assert.match(request.messages[1].content, /Thursday, January 15, 2026 at 12:08:09 PM/);
  assert.match(request.messages[1].content, /America\/New_York \(UTC-05:00\)/);
  assert.throws(() => createAssistantRuntime({ timeZone: "Not\/A_Timezone" }),
    /invalid assistant IANA time zone: Not\/A_Timezone/);
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

test("assistant readiness probes the configured model without generating chat", async () => {
  const requests = [];
  const runtime = createAssistantRuntime({
    enabled: true, baseUrl: "http://127.0.0.1:8181", model: "helix-qwen3-0.6b", logger: quietLogger,
    fetchImpl: async (url) => {
      requests.push(new URL(url).pathname);
      return jsonResponse({ data: [{ id: "helix-qwen3-0.6b" }] });
    },
  });
  assert.equal((await runtime.refreshHealth()).qwen, "online");
  assert.deepEqual(requests, ["/v1/models"]);
});

test("production hotwords only bias the spoken name Neo", async () => {
  const hotwords = await readFile(new URL("../config/newo-hotwords.txt", import.meta.url), "utf8");
  assert.equal(hotwords.replace(/\r\n/g, "\n"), "NEO\n");
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
