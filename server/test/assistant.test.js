import assert from "node:assert/strict";
import http from "node:http";
import test from "node:test";
import { readFile } from "node:fs/promises";
import { ASSISTANT_HISTORY_MAX_CHARS_PER_MESSAGE, ASSISTANT_SYSTEM_PROMPT, createAssistantRuntime, directClockShortcut } from "../src/assistant.js";
import { createAssistantProfiles, LFM_PROFILE_ID, LFM_SYSTEM_PROMPT, QWEN_PROFILE_ID } from "../src/assistant-profiles.js";
import { createAssistantTurnRuntime, createSpeechSegmenter } from "../src/assistant-turn.js";

const turn = { deviceId: "newo-01", streamId: "stream-1", text: "hello Newo" };
const quietLogger = { info() {}, warn() {} };

function jsonResponse(payload, status = 200) {
  return new Response(JSON.stringify(payload), { status, headers: { "content-type": "application/json" } });
}

function ndjsonResponse(parts, signal) {
  return new Response(new ReadableStream({
    async start(controller) {
      signal?.addEventListener("abort", () => controller.error(signal.reason), { once: true });
      for (const part of parts) {
        if (typeof part === "number") await new Promise((resolve) => setTimeout(resolve, part));
        else controller.enqueue(part);
      }
      if (!signal?.aborted) controller.close();
    },
  }), { status: 200, headers: { "content-type": "application/x-ndjson" } });
}

function ollamaText(text, signal) {
  return ndjsonResponse([new TextEncoder().encode(`${JSON.stringify({ response: text })}\n${JSON.stringify({ done: true, eval_count: 8, prompt_eval_count: 20 })}\n`)], signal);
}

function fakeWebTools(invoke) {
  return {
    available: true,
    definitions: [
      { name: "web_search", description: "Search current sources", parameters: { type: "object", properties: { query: { type: "string" }, max_results: { type: "integer" } }, required: ["query"], additionalProperties: false } },
      { name: "web_read", description: "Read one source", parameters: { type: "object", properties: { url: { type: "string" }, max_chars: { type: "integer" } }, required: ["url"], additionalProperties: false } },
    ],
    invoke,
  };
}

test("assistant sends one bounded OpenAI-compatible quick-chat request", async () => {
  let request;
  let requestUrl;
  const runtime = createAssistantRuntime({
    enabled: true,
    baseUrl: "http://127.0.0.1:8181",
    model: "helix-qwen3-0.6b",
    logger: quietLogger,
    fetchImpl: async (url, options) => {
      requestUrl = url;
      request = JSON.parse(options.body);
      return jsonResponse({ choices: [{ message: { content: "Hello. I am Neo." } }] });
    },
  });

  const result = await runtime.respond(turn);

  assert.equal(result.kind, "response");
  assert.equal(result.text, "Hello. I am Neo.");
  assert.equal(requestUrl, "http://127.0.0.1:8181/v1/chat/completions");
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
  assert.equal(request.stream, true);
  assert.equal(Object.hasOwn(request, "reasoning_effort"), false);
  assert.equal(result.timings.history_used, 0);
  assert.equal(runtime.getTelemetry().provider, "openai_chat");
});

function testProfiles() {
  const profiles = createAssistantProfiles();
  return {
    ...profiles,
    [LFM_PROFILE_ID]: { ...profiles[LFM_PROFILE_ID], baseUrl: "http://lfm.test" },
    [QWEN_PROFILE_ID]: { ...profiles[QWEN_PROFILE_ID], baseUrl: "http://qwen.test" },
  };
}

test("profile fallback retries once with the whole Qwen profile and reports the actual producer", async () => {
  const requests = [];
  const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
    fetchImpl: async (url, options) => {
      requests.push({ url, body: options.body && JSON.parse(options.body) });
      if (url === "http://lfm.test/api/generate") throw new Error("offline");
      return jsonResponse({ choices: [{ message: { content: "Qwen fallback reply." } }] });
    } });
  const result = await runtime.respond(turn);
  assert.equal(result.kind, "response");
  assert.deepEqual(requests.map((item) => item.url), ["http://lfm.test/api/generate", "http://qwen.test/v1/chat/completions"]);
  assert.equal(requests[1].body.messages[0].content, ASSISTANT_SYSTEM_PROMPT);
  assert.equal(Object.hasOwn(requests[1].body, "prompt"), false);
  assert.equal(result.timings.preferred_profile, LFM_PROFILE_ID);
  assert.equal(result.timings.effective_profile, QWEN_PROFILE_ID);
  assert.equal(result.timings.provider, "openai_chat");
  assert.equal(runtime.getTelemetry().fallback_active, true);
});

test("successful and empty preferred responses do not fallback", async () => {
  let calls = 0;
  const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
    fetchImpl: async (_url, options) => { calls += 1; return ndjsonResponse([new TextEncoder().encode('{"response":"Hello."}\n')], options.signal); } });
  assert.equal((await runtime.respond(turn)).kind, "response");
  assert.equal(calls, 1);
  const emptyRuntime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
    fetchImpl: async (_url, options) => { calls += 1; return ndjsonResponse([new TextEncoder().encode('{"response":"<think>hidden"}\n')], options.signal); } });
  assert.equal((await emptyRuntime.respond({ ...turn, deviceId: "empty" })).kind, "empty");
  assert.equal(calls, 2);
});

test("fallback is non-recursive when both providers are unavailable", async () => {
  let calls = 0;
  const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
    fetchImpl: async () => { calls += 1; throw new Error("offline"); } });
  assert.equal((await runtime.respond(turn)).kind, "error");
  assert.equal(calls, 2);
});

test("profile timeout falls back, while explicit cancellation never does", async () => {
  const profiles = testProfiles();
  profiles[LFM_PROFILE_ID] = { ...profiles[LFM_PROFILE_ID], timeoutMs: 10 };
  let fallbackCalls = 0;
  const timeoutRuntime = createAssistantRuntime({ enabled: true, profiles, preferredProfile: "lfm", logger: quietLogger,
    fetchImpl: async (url, options) => {
      if (url.includes("lfm.test")) return ndjsonResponse([100], options.signal);
      fallbackCalls += 1;
      return jsonResponse({ choices: [{ message: { content: "Recovered after timeout." } }] });
    } });
  const timeoutResult = await timeoutRuntime.respond(turn);
  assert.equal(timeoutResult.text, "Recovered after timeout.");
  assert.equal(fallbackCalls, 1);
  assert.equal(timeoutResult.timings.fallback_reason, "assistant_timeout");

  const cancelledRuntime = createAssistantRuntime({ enabled: true, profiles, preferredProfile: "lfm", logger: quietLogger,
    fetchImpl: async (_url, options) => ndjsonResponse([100], options.signal) });
  const pending = cancelledRuntime.respond({ ...turn, deviceId: "cancel-profile" });
  cancelledRuntime.abortDevice("cancel-profile");
  assert.deepEqual(await pending, { kind: "error", error: "assistant_cancelled" });
});

test("fallback recovers to the preferred profile after cooldown health succeeds", async () => {
  let lfmGenerateCalls = 0;
  const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm",
    fallbackCooldownMs: 0, logger: quietLogger, fetchImpl: async (url, options = {}) => {
      if (url === "http://lfm.test/api/tags") return jsonResponse({ models: [{ name: "newo-main:latest" }] });
      if (url === "http://lfm.test/api/generate") {
        lfmGenerateCalls += 1;
        if (lfmGenerateCalls === 1) throw new Error("offline");
        return ndjsonResponse([new TextEncoder().encode('{"response":"LFM recovered."}\n')], options.signal);
      }
      return jsonResponse({ choices: [{ message: { content: "fallback" } }] });
    } });
  await runtime.respond(turn);
  const recovered = await runtime.respond({ ...turn, streamId: "stream-2", text: "another question" });
  assert.equal(recovered.text, "LFM recovered.");
  assert.equal(runtime.getTelemetry().effective_profile, LFM_PROFILE_ID);
  assert.equal(runtime.getTelemetry().fallback_active, false);
});

test("ollama_raw FAST uses the closed-think bridge and parses arbitrary NDJSON and UTF-8 splits", async () => {
  let request;
  let requestUrl;
  const encoder = new TextEncoder();
  const runtime = createAssistantRuntime({
    enabled: true, provider: "ollama_raw", baseUrl: "http://100.68.131.86:11435/", model: "newo-main",
    maxOutputTokens: 48, logger: quietLogger,
    fetchImpl: async (url, options) => {
      requestUrl = url;
      request = JSON.parse(options.body);
      const bytes = encoder.encode('\n{"response":""}\n{"response":"<Th"}\n{"response":"InK>private"}\n{"response":" notes</tH"}\n{"response":"iNk> Hello 🌍"}\n{"response":" from Neo.","done":false}\n{"response":"","done":true}');
      const globe = bytes.indexOf(0xf0);
      return ndjsonResponse([bytes.slice(0, 5), bytes.slice(5, 31), bytes.slice(31, globe + 1), bytes.slice(globe + 1, globe + 3), bytes.slice(globe + 3)], options.signal);
    },
  });
  const result = await runtime.respond(turn);
  assert.equal(requestUrl, "http://100.68.131.86:11435/api/generate");
  assert.equal(result.text, "Hello 🌍 from Neo.");
  assert.equal(typeof result.timings.llm_first_raw_token_ms, "number");
  assert.equal(typeof result.timings.llm_first_token_ms, "number");
  assert.deepEqual(request, {
    model: "newo-main",
    prompt: `<|startoftext|><|im_start|>system\n${LFM_SYSTEM_PROMPT}\n<|im_end|>\n<|im_start|>user\n${turn.text}\n<|im_end|>\n<|im_start|>assistant\n<think>\nNo unnecessary reasoning. Close thinking and answer immediately.\n</think>\n`,
    raw: true, stream: true, keep_alive: -1,
    options: { num_predict: 48, temperature: 0.2, top_k: 80, repeat_penalty: 1.05, stop: ["<|im_end|>", "<|im_start|>"] },
  });
  assert.equal("num_ctx" in request.options, false);
  assert.doesNotMatch(request.prompt, /NO-THINK MODE/i);
  assert.match(request.prompt, /<think>[\s\S]*No unnecessary reasoning[\s\S]*<\/think>/);
});

test("ollama_raw THINK preserves native reasoning while Qwen keeps its own template", async () => {
  const requests = [];
  const profiles = testProfiles();
  const runtime = createAssistantRuntime({ enabled: true, profiles, preferredProfile: "lfm", logger: quietLogger,
    fetchImpl: async (url, options) => {
      requests.push({ url, body: JSON.parse(options.body) });
      return ndjsonResponse([new TextEncoder().encode('{"response":"<think>compare"}\n{"response":"</think>Use the safer reading."}\n{"done":true,"eval_count":9}')], options.signal);
    } });
  const result = await runtime.respond({ ...turn, text: "Sensor A says safe, but sensor B says unsafe." });
  assert.equal(result.timings.reasoning_route, "THINK");
  assert.equal(result.timings.reasoning_tokens, 1);
  assert.doesNotMatch(requests[0].body.prompt, /No unnecessary reasoning/);

  await runtime.setPreferredProfile("qwen");
  const qwenRuntime = createAssistantRuntime({ enabled: true, profiles, preferredProfile: "qwen", logger: quietLogger,
    fetchImpl: async (_url, options) => { requests.push({ url: _url, body: JSON.parse(options.body) });
      return jsonResponse({ choices: [{ message: { content: "Qwen answer." } }] }); } });
  const qwen = await qwenRuntime.respond({ ...turn, text: "Sensor A says safe, but sensor B says unsafe." });
  const body = requests.at(-1).body;
  assert.equal(qwen.timings.reasoning_route, "THINK");
  assert.ok(Array.isArray(body.messages));
  assert.equal("prompt" in body, false);
  assert.equal(body.temperature, profiles[QWEN_PROFILE_ID].sampling.temperature);
});

test("first raw token precedes the first speakable token hidden by think filtering", async () => {
  const encoder = new TextEncoder();
  const runtime = createAssistantRuntime({
    enabled: true, provider: "ollama_raw", baseUrl: "http://local", model: "model", logger: quietLogger,
    fetchImpl: async (_url, options) => ndjsonResponse([encoder.encode('{"response":"<think>hidden"}\n'), 20, encoder.encode('{"response":"</think>Useful"}\n{"done":true}')], options.signal),
  });
  const result = await runtime.respond(turn);
  assert.equal(result.text, "Useful");
  assert.ok(result.timings.llm_first_token_ms > result.timings.llm_first_raw_token_ms);
});

test("closed and unterminated think output never reaches TTS", async () => {
  const answers = ["<think>secret</think> Spoken.", "<THINK>unfinished secret"];
  const spoken = [];
  for (const [index, answer] of answers.entries()) {
    const encoder = new TextEncoder();
    const assistant = createAssistantRuntime({
      enabled: true, provider: "ollama_raw", baseUrl: "http://local", model: "model", logger: quietLogger,
      fetchImpl: async (_url, options) => ndjsonResponse([encoder.encode(`${JSON.stringify({ response: answer })}\n`)], options.signal),
    });
    const turns = createAssistantTurnRuntime({ assistant, speakerRuntime: { speak(text) { spoken.push(text); return { kind: "queued", playbackId: "p", completion: Promise.resolve() }; } }, isPersistentSpeakerEnabled: () => true, maxReplyChars: 300, logger: quietLogger });
    const result = await turns.handleFinalTranscript({ ...turn, streamId: `think-${index}` }).completion;
    assert.equal(result.kind, index === 0 ? "complete" : "empty");
  }
  assert.deepEqual(spoken, ["Spoken."]);
});

test("ollama_raw cancellation and timeout abort during body streaming", async () => {
  let bodyStarted;
  const started = new Promise((resolve) => { bodyStarted = resolve; });
  const encoder = new TextEncoder();
  const cancellation = createAssistantRuntime({
    enabled: true, provider: "ollama_raw", baseUrl: "http://local", model: "model", logger: quietLogger,
    fetchImpl: async (_url, { signal }) => new Response(new ReadableStream({ start(controller) { controller.enqueue(encoder.encode('{"response":"partial"}\n')); bodyStarted(); signal.addEventListener("abort", () => controller.error(signal.reason), { once: true }); } })),
  });
  const pending = cancellation.respond(turn);
  await started;
  assert.equal(cancellation.getTelemetry().active, true);
  cancellation.abortDevice(turn.deviceId);
  assert.deepEqual(await pending, { kind: "error", error: "assistant_cancelled" });
  assert.equal(cancellation.getTelemetry().active, false);

  const timeout = createAssistantRuntime({
    enabled: true, provider: "ollama_raw", baseUrl: "http://local", model: "model", timeoutMs: 10, logger: quietLogger,
    fetchImpl: async (_url, { signal }) => new Response(new ReadableStream({ start(controller) { signal.addEventListener("abort", () => controller.error(signal.reason), { once: true }); } })),
  });
  assert.equal((await timeout.respond(turn)).kind, "timeout");
});

test("ollama_raw reports stream errors and provider/model health", async () => {
  const encoder = new TextEncoder();
  const failed = createAssistantRuntime({ enabled: true, provider: "ollama_raw", baseUrl: "http://local", model: "model", logger: quietLogger,
    fetchImpl: async (_url, options) => ndjsonResponse([encoder.encode('{"error":"model unavailable"}\n')], options.signal) });
  assert.deepEqual(await failed.respond(turn), { kind: "error", error: "assistant_request_failed" });

  const healthy = createAssistantRuntime({ enabled: true, provider: "ollama_raw", baseUrl: "http://local", model: "newo-main", logger: quietLogger,
    fetchImpl: async () => jsonResponse({ models: [{ name: "newo-main:latest" }] }) });
  const telemetry = await healthy.refreshHealth();
  assert.deepEqual({ provider: telemetry.provider, model: telemetry.model, online: telemetry.online }, { provider: "ollama_raw", model: "newo-main", online: "online" });
});

test("ollama_raw reuses one persistent HTTP connection across health and turns", async () => {
  const sockets = new Set();
  const server = http.createServer((request, response) => {
    sockets.add(request.socket);
    response.setHeader("content-type", "application/x-ndjson");
    if (request.url === "/api/tags") response.end(JSON.stringify({ models: [{ name: "newo-main" }] }));
    else response.end('{"response":"Ready."}\n{"done":true}\n');
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const runtime = createAssistantRuntime({ enabled: true, provider: "ollama_raw", baseUrl: `http://127.0.0.1:${server.address().port}`, model: "newo-main", logger: quietLogger });
  try {
    await runtime.refreshHealth();
    await runtime.respond(turn);
    await runtime.respond({ ...turn, streamId: "stream-2" });
    assert.equal(sockets.size, 1);
  } finally {
    runtime.close();
    await new Promise((resolve) => server.close(resolve));
  }
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

test("direct clock shortcuts speak midnight, noon, and minutes naturally in Asia/Phnom_Penh", () => {
  const zone = "Asia/Phnom_Penh";
  assert.equal(directClockShortcut("what time is it", new Date("2026-09-12T17:00:00Z"), zone).text, "It's twelve AM.");
  assert.equal(directClockShortcut("what's the time", new Date("2026-09-13T05:00:00Z"), zone).text, "It's twelve PM.");
  assert.equal(directClockShortcut("current time", new Date("2026-09-13T06:05:00Z"), zone).text, "It's one oh five PM.");
  assert.equal(directClockShortcut("what is the time", new Date("2026-09-13T06:15:00Z"), zone).text, "It's one fifteen PM.");
  assert.equal(directClockShortcut("what time is it with seconds", new Date("2026-09-13T06:05:42Z"), zone).text, "It's one oh five PM and forty two seconds.");
});

test("clear direct clock intents bypass the LLM", async () => {
  let fetchCalls = 0;

  const runtime = createAssistantRuntime({
    enabled: true,
    baseUrl: "http://local",
    model: "model",
    timeZone: "Asia/Phnom_Penh",
    now: () => new Date("2026-09-13T06:05:42.000Z"),
    logger: quietLogger,
    fetchImpl: async () => { fetchCalls += 1; throw new Error("LLM must not be called"); },
  });

  const result = await runtime.respond({ ...turn, streamId: "time-1", text: "What time is it?" });
  assert.equal(result.text, "It's one oh five PM.");
  assert.equal(result.timings.route, "local_time");
  assert.equal(result.timings.llm_request_ms, 0);
  assert.equal(fetchCalls, 0);
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

  await runtime.respond({ ...turn, text: "What date is it?" });

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
  assert.equal((await runtime.refreshHealth()).online, "online");
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
    getTelemetry() { return { enabled: true, model: "helix-qwen3-0.6b", online: "online", active: false }; },
  };
  const speakerRuntime = { speak() { return { kind: "queued", playbackId: "p", completion: Promise.resolve() }; } };
  const turns = createAssistantTurnRuntime({ assistant, speakerRuntime, isPersistentSpeakerEnabled: () => true, maxReplyChars: 240, logger: quietLogger });
  assert.deepEqual(turns.getTelemetry().latest, { result: "n/a", llmMs: null, llmFirstRawTokenMs: null, llmFirstTokenMs: null,
    llmFirstTtsChunkMs: null, llmFirstAudioMs: null, streamId: null, at: null, ttsQueuedMs: null, totalMs: null, asrFinalMs: null,
    progressFeedbackFired: false, progressFeedbackStartedMs: null, progressFeedbackFinishedMs: null });
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

test("progressive chunking waits for useful sentence or clause boundaries", () => {
  const chunks = [];
  const segmenter = createSpeechSegmenter({ onSegment: (text) => chunks.push(text) });
  for (const part of ["Here is a short", " opening. Now a longer clause", ", with useful detail that keeps", " speech natural."])
    segmenter.push(part);
  segmenter.finish();
  assert.deepEqual(chunks, ["Here is a short opening.", "Now a longer clause, with useful detail that keeps speech natural."]);
});

test("LFM progressive speech keeps the selected 450-character profile budget", async () => {
  const spoken = [];
  const longReply = `${"A".repeat(149)}. ${"B".repeat(149)}. ${"C".repeat(149)}.`;
  const runtime = createAssistantTurnRuntime({
    assistant: { getTelemetry: () => ({ enabled: true }), async respond({ onSpeakableText }) {
      onSpeakableText(longReply, { profileId: "lfm2.5:8b", maxReplyChars: 450, chunking: { minChars: 24, clauseChars: 72, hardChars: 160 } });
      return { kind: "response", text: longReply, timings: {} };
    } },
    speakerRuntime: {
      speakProgressive(segments) { void (async () => { for await (const text of segments) spoken.push(text); })(); return { kind: "queued", playbackId: "p", completion: Promise.resolve({}) }; },
      speak() { throw new Error("progressive path expected"); },
    },
    isPersistentSpeakerEnabled: () => true, maxReplyChars: 300, logger: { info() {}, warn() {} },
  });
  const turn = runtime.handleFinalTranscript({ deviceId: "d", streamId: "s", text: "long answer", asrFinalMs: 1 });
  await turn.completion;
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(spoken.join(" ").length, 450);
});

test("OpenAI-compatible SSE and Ollama deltas use the same progressive turn pipeline", async () => {
  for (const profile of ["qwen", "lfm"]) {
    const profiles = testProfiles();
    const encoder = new TextEncoder();
    const runtime = createAssistantRuntime({ enabled: true, profiles, preferredProfile: profile, logger: quietLogger,
      fetchImpl: async (_url, options) => profile === "lfm"
        ? ndjsonResponse([encoder.encode('{"response":"A useful opening. "}\n{"response":"More helpful detail follows."}\n')], options.signal)
        : new Response(new ReadableStream({ start(controller) {
          controller.enqueue(encoder.encode('data: {"choices":[{"delta":{"content":"A useful opening. "}}]}\n\n'));
          controller.enqueue(encoder.encode('data: {"choices":[{"delta":{"content":"More helpful detail follows."}}]}\n\ndata: [DONE]\n\n'));
          controller.close();
        } }), { headers: { "content-type": "text/event-stream" } }) });
    const spoken = [];
    const speaker = {
      speak() { throw new Error("complete-response TTS path must not be used"); },
      speakProgressive(segments) {
        const completion = (async () => { for await (const text of segments) spoken.push(text); return { kind: "complete" }; })();
        return { kind: "queued", playbackId: `${profile}-p`, completion };
      },
    };
    const turns = createAssistantTurnRuntime({ assistant: runtime, speakerRuntime: speaker,
      isPersistentSpeakerEnabled: () => true, maxReplyChars: 300, logger: quietLogger });
    assert.equal((await turns.handleFinalTranscript(turn).completion).kind, "complete");
    assert.deepEqual(spoken, ["A useful opening. More helpful detail follows."]);
    assert.equal(typeof turns.getTelemetry().latest.llmFirstTtsChunkMs, "number");
  }
});

test("assistant turn state follows LLM start, first useful token, and completion", async () => {
  const states = [];
  let finishPlayback;
  const playback = new Promise((resolve) => { finishPlayback = resolve; });
  const assistant = {
    async respond(request) {
      request.onFirstToken();
      return { kind: "response", text: "Hello.", timings: { llm_request_ms: 2, llm_first_token_ms: 1 } };
    },
    abortDevice() {}, close() {},
  };
  const speaker = { speak() { return { kind: "queued", playbackId: "p", completion: playback }; } };
  const turns = createAssistantTurnRuntime({ assistant, speakerRuntime: speaker,
    isPersistentSpeakerEnabled: () => true, maxReplyChars: 240, logger: quietLogger,
    setAssistantState: (_deviceId, state) => states.push(state) });
  const started = turns.handleFinalTranscript(turn);
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(states, ["thinking", "responding"]);
  finishPlayback({ kind: "complete" });
  await started.completion;
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(states, ["thinking", "responding", "idle"]);
});

test("LFM stable facts can answer without invoking web tools", async () => {
  let toolCalls = 0;
  const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
    webTools: fakeWebTools(async () => { toolCalls += 1; }),
    fetchImpl: async (_url, options) => ollamaText("Paris is the capital of France.", options.signal) });
  const result = await runtime.respond({ ...turn, text: "What is the capital of France?" });
  assert.equal(result.text, "Paris is the capital of France.");
  assert.equal(toolCalls, 0);
  assert.equal(result.timings.llm_rounds, 1);
});

test("capability routing runs once and restricts LFM tool definitions without changing reasoning route", async () => {
  const decisions = [
    { primary: "knowledge", raw_primary: "knowledge", abstain: false, fallback: false, source_need: "stable", confidence: 0.9, margin: 0.8, latency_ms: 5, request_latency_ms: 7 },
    { primary: "web.search", raw_primary: "web.search", abstain: false, fallback: false, source_need: "live", confidence: 0.8, margin: 0.6, latency_ms: 5, request_latency_ms: 7 },
    { primary: "weather.current", raw_primary: "weather.current", abstain: false, fallback: false, source_need: "live", confidence: 0.85, margin: 0.7, latency_ms: 5, request_latency_ms: 7 },
    { primary: null, raw_primary: "__abstain__", abstain: true, fallback: false, source_need: "unknown", confidence: 0.7, margin: 0.4, latency_ms: 5, request_latency_ms: 7 },
    { fallback: true, reason: "capability_router_timeout", latency_ms: null, request_latency_ms: 75 },
  ];
  const expected = [[], ["web_search"], ["web_search", "web_read"], ["web_search", "web_read"], ["web_search", "web_read"]];
  for (let index = 0; index < decisions.length; index += 1) {
    let inferenceCalls = 0;
    let prompt = "";
    const capabilityRouter = { async classify() { inferenceCalls += 1; return decisions[index]; } };
    const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
      capabilityRouter, webTools: fakeWebTools(async () => { throw new Error("not expected"); }),
      fetchImpl: async (_url, options) => { prompt = JSON.parse(options.body).prompt; return ollamaText("A direct answer.", options.signal); } });
    const result = await runtime.respond({ ...turn, deviceId: `cap-${index}`, text: "Explain the topic clearly." });
    assert.equal(inferenceCalls, 1);
    assert.equal(result.timings.reasoning_route, "FAST");
    const exposed = ["web_search", "web_read"].filter((name) => prompt.includes(`\"name\":\"${name}\"`));
    assert.deepEqual(exposed, expected[index]);
    assert.equal(result.timings.capability_router_fallback, Boolean(decisions[index].fallback));
  }
});

test("LFM explicit and current requests execute validated native web_search calls", async () => {
  for (const text of ["Search the web for current Node.js news.", "What is the latest Node.js release?"]) {
    const invoked = [];
    let round = 0;
    const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
      webTools: fakeWebTools(async (name, args) => { invoked.push({ name, args }); return { value: { results: [{ url: "https://nodejs.org", fetched_at: "2026-09-14T00:00:00Z" }] }, elapsedMs: 12, providerElapsedMs: 10 }; }),
      fetchImpl: async (_url, options) => ollamaText(++round === 1
        ? '<|tool_call_start|>[web_search(query="latest Node.js release", max_results=3)]<|tool_call_end|>'
        : "The current release is documented by Node.js.", options.signal) });
    const result = await runtime.respond({ ...turn, deviceId: text, text });
    assert.deepEqual(invoked, [{ name: "web_search", args: { query: "latest Node.js release", max_results: 3 } }]);
    assert.equal(result.timings.tool_successes, 1);
    assert.equal(result.timings.llm_rounds, 2);
    assert.match(result.text, /current release/);
  }
});

test("source-reading request completes a native search then read loop", async () => {
  const invoked = [];
  const outputs = [
    '<|tool_call_start|>[web_search(query="Node.js release notes") ]<|tool_call_end|>',
    '<|tool_call_start|>[web_read(url="https://nodejs.org/release", max_chars=4000)]<|tool_call_end|>',
    "The release notes report the requested change.",
  ];
  const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
    webTools: fakeWebTools(async (name) => { invoked.push(name); return { value: name === "web_search"
      ? { results: [{ url: "https://nodejs.org/release", fetched_at: "2026-09-14T00:00:00Z" }] }
      : { url: "https://nodejs.org/release", content: "release notes", fetched_at: "2026-09-14T00:00:01Z" }, elapsedMs: 5, providerElapsedMs: null }; }),
    fetchImpl: async (_url, options) => ollamaText(outputs.shift(), options.signal) });
  const result = await runtime.respond({ ...turn, text: "Find and read the Node.js release notes." });
  assert.deepEqual(invoked, ["web_search", "web_read"]);
  assert.equal(result.timings.llm_rounds, 3);
  assert.equal(result.timings.tool_events.length, 2);
});

test("malformed native tool calls are rejected without execution", async () => {
  let invoked = 0;
  const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
    webTools: fakeWebTools(async () => { invoked += 1; }),
    fetchImpl: async (_url, options) => ollamaText('<|tool_call_start|>[web_search(query="x")', options.signal) });
  const result = await runtime.respond({ ...turn, text: "Search for x." });
  assert.equal(result.kind, "error");
  assert.equal(result.error, "assistant_tool_protocol_invalid");
  assert.equal(invoked, 0);
});

test("failed live retrieval is reported to LFM but cannot become a fabricated current answer", async () => {
  const outputs = [
    '<|tool_call_start|>[web_search(query="current event") ]<|tool_call_end|>',
    "The current event definitely happened today.",
  ];
  const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
    webTools: fakeWebTools(async () => { const error = new Error("timeout"); error.code = "agent_tools_timeout"; throw error; }),
    fetchImpl: async (_url, options) => ollamaText(outputs.shift(), options.signal) });
  const result = await runtime.respond({ ...turn, text: "What happened today?" });
  assert.equal(result.text, "I couldn't verify that from live sources right now.");
  assert.equal(result.timings.tool_successes, 0);
  assert.equal(result.timings.tool_events[0].error, "agent_tools_timeout");
});

test("repeated native tool loops are bounded to two searches and five rounds", async () => {
  let invoked = 0;
  let rounds = 0;
  const runtime = createAssistantRuntime({ enabled: true, profiles: testProfiles(), preferredProfile: "lfm", logger: quietLogger,
    webTools: fakeWebTools(async () => { invoked += 1; return { value: { results: [] }, elapsedMs: 1, providerElapsedMs: null }; }),
    fetchImpl: async (_url, options) => { rounds += 1; return ollamaText(`<|tool_call_start|>[web_search(query="loop ${rounds}")]<|tool_call_end|>`, options.signal); } });
  const result = await runtime.respond({ ...turn, text: "Keep searching forever." });
  assert.equal(result.kind, "error");
  assert.equal(result.error, "assistant_tool_loop_limit");
  assert.equal(invoked, 2);
  assert.equal(rounds, 5);
});

test("slow THINK work gives one delayed activity acknowledgement without colliding with the answer", async () => {
  const calls = [];
  const assistant = {
    routeRequest() { return { route: "THINK", reasons: ["conflicting_evidence"], activity: "sensor_fusion" }; },
    async respond(request) {
      await new Promise((resolve) => setTimeout(resolve, 825));
      request.onFirstToken();
      return { kind: "response", text: "Use the second sensor.", timings: { llm_request_ms: 825,
        reasoning_route: "THINK", routing_reasons: ["conflicting_evidence"], activity: "sensor_fusion", reasoning_tokens: 12 } };
    }, abortDevice() {}, close() {}, getTelemetry() { return { enabled: true }; },
  };
  const speaker = { speak(text) { calls.push(text); return { kind: "queued", playbackId: `p${calls.length}`, completion: Promise.resolve({}) }; } };
  const turns = createAssistantTurnRuntime({ assistant, speakerRuntime: speaker, isPersistentSpeakerEnabled: () => true,
    maxReplyChars: 300, progressFeedbackEnabled: true, logger: quietLogger });
  assert.equal((await turns.handleFinalTranscript(turn).completion).kind, "complete");
  assert.deepEqual(calls, ["Checking the readings together.", "Use the second sensor."]);
  assert.equal(turns.getTelemetry().latest.progressFeedbackFired, true);
  assert.ok(turns.getTelemetry().latest.progressFeedbackStartedMs >= 790);
  assert.equal(turns.getTelemetry().latest.reasoningRoute, "THINK");
  assert.equal(turns.getTelemetry().latest.reasoningTokens, 12);
});

test("real speakable output before the threshold suppresses progress speech", async () => {
  let progressCalls = 0;
  const assistant = {
    routeRequest() { return { route: "THINK", reasons: ["multi_step_structure"], activity: "search" }; },
    async respond(request) { request.onFirstToken(); return { kind: "response", text: "Ready.", timings: {} }; },
    abortDevice() {}, close() {}, getTelemetry() { return { enabled: true }; },
  };
  const speaker = { speak() { progressCalls += 1; return { kind: "queued", playbackId: "p", completion: Promise.resolve({}) }; } };
  const turns = createAssistantTurnRuntime({ assistant, speakerRuntime: speaker, isPersistentSpeakerEnabled: () => true,
    maxReplyChars: 300, logger: quietLogger });
  await turns.handleFinalTranscript(turn).completion;
  await new Promise((resolve) => setTimeout(resolve, 810));
  assert.equal(progressCalls, 1);
  assert.equal(turns.getTelemetry().latest.progressFeedbackFired, false);
});

for (const [label, toolDelay, expectedProgress] of [["fast", 20, false], ["slow", 830, true]]) {
  test(`${label} web search ${expectedProgress ? "fires one" : "cancels"} delayed progress phrase`, async () => {
    const spoken = [];
    const assistant = {
      getTelemetry: () => ({ enabled: true, web_tools: true }),
      routeRequest: () => ({ route: "THINK", reasons: ["open_world_tool_hint"], activity: "search", webTools: true }),
      async respond(request) {
        await request.onToolStart({ name: "web_search", round: 1 });
        await new Promise((resolve) => setTimeout(resolve, toolDelay));
        await request.onToolEnd({ name: "web_search", round: 1, event: { ok: true } });
        request.onFirstToken();
        return { kind: "response", text: "Verified final answer.", timings: { llm_request_ms: toolDelay + 5,
          tool_selected: ["web_search"], tool_events: [{ tool: "web_search", ok: true }] } };
      }, cancelDevice() {}, close() {},
    };
    const speaker = { speak(text) { spoken.push(text); return { kind: "queued", playbackId: `p-${spoken.length}`, completion: Promise.resolve({}) }; } };
    const turns = createAssistantTurnRuntime({ assistant, speakerRuntime: speaker, isPersistentSpeakerEnabled: () => true,
      maxReplyChars: 300, progressFeedbackEnabled: true, logger: quietLogger });
    await turns.handleFinalTranscript({ ...turn, streamId: `web-${label}` }).completion;
    assert.deepEqual(spoken, expectedProgress
      ? ["Checking current sources.", "Verified final answer."]
      : ["Verified final answer."]);
    assert.equal(turns.getTelemetry().latest.progressFeedbackFired, expectedProgress);
    assert.equal(turns.getTelemetry().latest.progressFeedbackCancelled, !expectedProgress);
  });
}

test("reachable new voice streams invalidate stale generation callbacks and permit a rapid next turn", async () => {
  const requests = [];
  const cancelled = [];
  const assistant = {
    respond(request) { return new Promise((resolve) => requests.push({ request, resolve })); },
    cancelDevice(deviceId) { cancelled.push(deviceId); }, abortDevice() {}, close() {},
  };
  const spoken = [];
  const speaker = {
    speakProgressive(segments, options) {
      const state = { cancelled: false, options };
      spoken.push(state);
      const completion = (async () => { for await (const _text of segments) {} return { kind: "complete" }; })();
      return { kind: "queued", playbackId: `p-${spoken.length}`, completion, cancel() { state.cancelled = true; } };
    },
    speak() { throw new Error("unexpected complete TTS"); },
  };
  const states = [];
  const turns = createAssistantTurnRuntime({ assistant, speakerRuntime: speaker,
    isPersistentSpeakerEnabled: () => true, maxReplyChars: 300, logger: quietLogger,
    setAssistantState: (_deviceId, state) => states.push(state) });
  const first = turns.handleFinalTranscript({ ...turn, streamId: "first" });
  requests[0].request.onSpeakableText("This first response is already speaking. ", { chunking: {} });
  turns.interruptDevice(turn.deviceId);
  requests[0].request.onSpeakableText("Stale text must never be queued.", { chunking: {} });
  requests[0].resolve({ kind: "response", text: "stale", timings: {} });
  assert.equal((await first.completion).kind, "cancelled");
  assert.equal(spoken[0].cancelled, true);

  const second = turns.handleFinalTranscript({ ...turn, streamId: "second" });
  requests[1].request.onSpeakableText("The second response is current and useful.", { chunking: {} });
  requests[1].resolve({ kind: "response", text: "The second response is current and useful.", timings: {} });
  assert.equal((await second.completion).kind, "complete");
  assert.equal(spoken.length, 2);
  assert.ok(spoken[1].options.metadata.generation_id > spoken[0].options.metadata.generation_id);
  assert.deepEqual(cancelled, [turn.deviceId]);
  assert.equal(states.includes("listening"), true);
});
