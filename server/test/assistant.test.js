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
    enabled: true,
    baseUrl: "http://127.0.0.1:8181",
    model: "helix-qwen3-0.6b",
    logger: quietLogger,
    fetchImpl: async (_url, options) => {
      request = JSON.parse(options.body);
      return jsonResponse({ choices: [{ message: { content: "Hello. I am Neo." } }] });
    },
  });

  const result = await runtime.respond(turn);

  assert.equal(result.kind, "response");
  assert.equal(result.text, "Hello. I am Neo.");
  assert.equal(request.model, "helix-qwen3-0.6b");
  assert.equal(request.messages.length, 2);
  assert.equal(request.messages[0].role, "system");
  assert.equal(request.messages[0].content, ASSISTANT_SYSTEM_PROMPT);
  assert.equal(request.messages[1].role, "user");
  assert.equal(request.messages[1].content, turn.text);
  assert.equal(request.temperature, 0.7);
  assert.equal(request.top_p, 0.8);
  assert.equal(request.top_k, 20);
  assert.equal(request.min_p, 0);
  assert.equal(request.max_tokens, 72);
  assert.equal(Object.hasOwn(request, "reasoning_effort"), false);
  assert.equal(result.timings.history_used, 0);
});

test("assistant retains only three bounded exchanges per device and clears them with the session", async () => {
  const requests = [];
  let reply = 0;

  const runtime = createAssistantRuntime({
    enabled: true,
    baseUrl: "http://local",
    model: "model",
    logger: quietLogger,
    fetchImpl: async (_url, options) => {
      requests.push(JSON.parse(options.body));
      reply += 1;
      return jsonResponse({
        choices: [{ message: { content: `answer ${reply} ${"x".repeat(300)}` } }],
      });
    },
  });

  for (let index = 1; index <= 5; index += 1) {
    await runtime.respond({
      deviceId: "device-a",
      streamId: `a-${index}`,
      text: `question ${index} ${"y".repeat(300)}`,
    });
  }

  assert.equal(requests[4].messages.length, 2);

  const followup = await runtime.respond({
    deviceId: "device-a",
    streamId: "followup",
    text: "tell me more",
  });

  assert.equal(followup.timings.history_available, 3);
  assert.equal(followup.timings.history_used, 1);

  const request = requests[5];
  assert.deepEqual(
    request.messages.map((message) => message.role),
    ["system", "user", "assistant", "user"],
  );
  assert.match(request.messages[1].content, /^question 5 /);
  assert.match(request.messages[2].content, /^answer 5 /);
  assert.ok(request.messages[1].content.length <= ASSISTANT_HISTORY_MAX_CHARS_PER_MESSAGE);
  assert.ok(request.messages[2].content.length <= ASSISTANT_HISTORY_MAX_CHARS_PER_MESSAGE);

  const other = await runtime.respond({
    deviceId: "device-b",
    streamId: "b-1",
    text: "tell me more",
  });

  assert.equal(other.timings.history_available, 0);
  assert.equal(other.timings.history_used, 0);

  runtime.abortDevice("device-a");

  const cleared = await runtime.respond({
    deviceId: "device-a",
    streamId: "a-new",
    text: "tell me more",
  });

  assert.equal(cleared.timings.history_available, 0);
  assert.equal(cleared.timings.history_used, 0);
});

test("assistant includes compact known runtime state and backend usage telemetry", async () => {
  let request;
  const logs = [];

  const runtime = createAssistantRuntime({
    enabled: true,
    baseUrl: "http://local",
    model: "model",
    runtimeContext: () => ({
      speakerEnabled: true,
      speakerVolume: 72,
      speakerMuted: false,
      cloudStatus: "connected",
    }),
    logger: { info(fields) { logs.push(fields); }, warn() {} },
    fetchImpl: async (_url, options) => {
      request = JSON.parse(options.body);
      return jsonResponse({
        choices: [{ message: { content: "The speaker volume is 72 percent." } }],
        usage: { prompt_tokens: 123, completion_tokens: 4 },
      });
    },
  });

  const result = await runtime.respond({
    ...turn,
    text: "What is your speaker volume and cloud status?",
  });

  assert.match(request.messages[0].content,
    /Current runtime state: speaker=enabled, volume=72%, mute=off, cloud=connected/);
  assert.equal(result.timings.input_tokens, 123);
  assert.equal(result.timings.output_tokens, 4);
  assert.equal(result.timings.history_used, 0);
  assert.equal(logs.at(-1).runtime_context, true);
});

test("assistant supplies current server time in the configured IANA timezone per turn", async () => {
  const requests = [];
  const clockValues = [
    new Date("2026-09-12T21:35:12.000Z"),
    new Date("2026-09-12T21:35:13.000Z"),
  ];

  const runtime = createAssistantRuntime({
    enabled: true,
    baseUrl: "http://local",
    model: "model",
    timeZone: "Asia/Phnom_Penh",
    now: () => clockValues.shift(),
    logger: quietLogger,
    fetchImpl: async (_url, options) => {
      requests.push(JSON.parse(options.body));
      return jsonResponse({
        choices: [{ message: { content: "It is four thirty-five AM." } }],
      });
    },
  });

  await runtime.respond({ ...turn, streamId: "time-1", text: "What time is it?" });
  await runtime.respond({ ...turn, streamId: "time-2", text: "What time is it now?" });

  assert.match(requests[0].messages[0].content,
    /Sunday, September 13, 2026 at 4:35:12 AM/);
  assert.match(requests[0].messages[0].content,
    /Asia\/Phnom_Penh \(UTC\+07:00\)/);
  assert.match(requests[1].messages[0].content, /4:35:13 AM/);
});

test("assistant respects another requested timezone and rejects invalid IANA zones", async () => {
  let request;

  const runtime = createAssistantRuntime({
    enabled: true,
    baseUrl: "http://local",
    model: "model",
    timeZone: "America/New_York",
    now: () => new Date("2026-01-15T17:08:09.000Z"),
    logger: quietLogger,
    fetchImpl: async (_url, options) => {
      request = JSON.parse(options.body);
      return jsonResponse({ choices: [{ message: { content: "It is noon." } }] });
    },
  });

  await runtime.respond({ ...turn, text: "What time is it?" });

  assert.match(request.messages[0].content,
    /Thursday, January 15, 2026 at 12:08:09 PM/);
  assert.match(request.messages[0].content,
    /America\/New_York \(UTC-05:00\)/);

  assert.throws(
    () => createAssistantRuntime({ timeZone: "Not/A_Timezone" }),
    /invalid assistant IANA time zone/,
  );
});

test("assistant identifies Newo as pronounced Neo", async () => {
  let request;

  const runtime = createAssistantRuntime({
    enabled: true,
    baseUrl: "http://local",
    model: "model",
    logger: quietLogger,
    fetchImpl: async (_url, options) => {
      request = JSON.parse(options.body);
      return jsonResponse({ choices: [{ message: { content: "My name is Neo." } }] });
    },
  });

  const result = await runtime.respond({ ...turn, text: "What is your name?" });

  assert.equal(result.text, "My name is Neo.");
  assert.match(ASSISTANT_SYSTEM_PROMPT, /Newo, pronounced Neo/);
  assert.match(ASSISTANT_SYSTEM_PROMPT, /I, me, and my mean the user/);
  assert.equal(request.messages[0].content, ASSISTANT_SYSTEM_PROMPT);
});

test("conversation meta questions use deterministic perspective-safe memory", async () => {
  let fetchCalls = 0;

  const runtime = createAssistantRuntime({
    enabled: true,
    baseUrl: "http://local",
    model: "model",
    logger: quietLogger,
    fetchImpl: async () => {
      fetchCalls += 1;
      return jsonResponse({
        choices: [{
          message: {
            content: fetchCalls === 1
              ? "Japan is an island country in East Asia."
              : "Vietnam is a country in Southeast Asia.",
          },
        }],
      });
    },
  });

  await runtime.respond({
    deviceId: "newo-01",
    streamId: "facts-1",
    text: "Tell me about Japan",
  });

  await runtime.respond({
    deviceId: "newo-01",
    streamId: "facts-2",
    text: "What about Vietnam",
  });

  const result = await runtime.respond({
    deviceId: "newo-01",
    streamId: "memory-1",
    text: "What did I just ask you?",
  });

  assert.equal(fetchCalls, 2);
  assert.equal(result.text, "You just asked: What about Vietnam");
  assert.equal(result.timings.llm_request_ms, 0);
  assert.equal(result.timings.route, "memory_user");
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
