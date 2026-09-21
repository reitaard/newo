import assert from "node:assert/strict";
import test from "node:test";

import { createAssistantRuntime } from "../src/assistant.js";
import { createAssistantProfiles, GEMMA_PROFILE_ID, QWEN_PROFILE_ID, resolveAssistantProfile } from "../src/assistant-profiles.js";

const quietLogger = { info() {}, warn() {} };
const turn = { deviceId: "newo-01", streamId: "gemma-1", text: "hello" };

function jsonResponse(payload, status = 200) {
  return new Response(JSON.stringify(payload), { status, headers: { "content-type": "application/json" } });
}

function ndjsonResponse(parts, signal) {
  return new Response(new ReadableStream({
    start(controller) {
      signal?.addEventListener("abort", () => controller.error(signal.reason), { once: true });
      for (const part of parts) controller.enqueue(new TextEncoder().encode(JSON.stringify(part) + "\n"));
      if (!signal?.aborted) controller.close();
    },
  }), { status: 200, headers: { "content-type": "application/x-ndjson" } });
}

function gemmaProfiles(overrides = {}) {
  const profiles = createAssistantProfiles({ overrides });
  return {
    ...profiles,
    [GEMMA_PROFILE_ID]: { ...profiles[GEMMA_PROFILE_ID], baseUrl: "http://gemma.test" },
    [QWEN_PROFILE_ID]: { ...profiles[QWEN_PROFILE_ID], baseUrl: "http://qwen.test" },
  };
}

test("Gemma profile uses persistent laptop endpoint and native Ollama chat", () => {
  const profile = createAssistantProfiles()[GEMMA_PROFILE_ID];
  assert.equal(resolveAssistantProfile("gemma"), GEMMA_PROFILE_ID);
  assert.equal(profile.provider, "ollama_chat");
  assert.equal(profile.baseUrl, "http://100.110.136.15:11435");
  assert.equal(profile.endpoint, "/api/chat");
  assert.equal(profile.model, "newo-gemma-e2b:latest");
  assert.equal(profile.thinkMode, "auto");
  assert.equal(profile.fallbackProfile, QWEN_PROFILE_ID);
});

test("Gemma FAST sends think false and never speaks message.thinking", async () => {
  let body;
  const runtime = createAssistantRuntime({ enabled: true, profiles: gemmaProfiles(), preferredProfile: "gemma", logger: quietLogger,
    fetchImpl: async (url, options = {}) => {
      assert.equal(url, "http://gemma.test/api/chat");
      body = JSON.parse(options.body);
      return ndjsonResponse([
        { message: { role: "assistant", thinking: "private chain" }, done: false },
        { message: { role: "assistant", content: "Visible answer." }, done: false },
        { message: { role: "assistant", content: "" }, done: true, prompt_eval_count: 20, eval_count: 9 },
      ], options.signal);
    } });
  const spoken = [];
  const result = await runtime.respond({ ...turn, onSpeakableText: (text) => spoken.push(text) });
  assert.equal(result.kind, "response");
  assert.equal(result.text, "Visible answer.");
  assert.equal(body.think, false);
  assert.ok(Array.isArray(body.messages));
  assert.deepEqual(spoken, ["Visible answer."]);
});

test("Gemma THINK route enables native thinking while content remains separate", async () => {
  let body;
  const runtime = createAssistantRuntime({ enabled: true, profiles: gemmaProfiles(), preferredProfile: "gemma", logger: quietLogger,
    fetchImpl: async (_url, options = {}) => {
      body = JSON.parse(options.body);
      return ndjsonResponse([{ message: { content: "Use the safer reading." }, done: true, eval_count: 12 }], options.signal);
    } });
  const result = await runtime.respond({ ...turn, text: "Sensor A says safe, but sensor B says unsafe." });
  assert.equal(result.kind, "response");
  assert.equal(result.timings.reasoning_route, "THINK");
  assert.equal(result.timings.think_enabled, true);
  assert.equal(body.think, true);
});

test("Gemma think override supports on/off/auto", async () => {
  for (const [mode, expected] of [["on", true], ["off", false]]) {
    let body;
    const runtime = createAssistantRuntime({ enabled: true, profiles: gemmaProfiles({ [GEMMA_PROFILE_ID]: { think_mode: mode } }), preferredProfile: "gemma", logger: quietLogger,
      fetchImpl: async (_url, options = {}) => { body = JSON.parse(options.body); return ndjsonResponse([{ message: { content: "ok" }, done: true }], options.signal); } });
    await runtime.respond(turn);
    assert.equal(body.think, expected);
    assert.equal(runtime.getPreferredProfileConfig().think_mode, mode);
  }
});

test("Gemma availability failure falls back to Qwen", async () => {
  const requests = [];
  const runtime = createAssistantRuntime({ enabled: true, profiles: gemmaProfiles(), preferredProfile: "gemma", logger: quietLogger,
    fetchImpl: async (url, options = {}) => {
      requests.push(url);
      if (url === "http://gemma.test/api/chat") throw new Error("offline");
      return jsonResponse({ choices: [{ message: { content: "Qwen fallback." } }] });
    } });
  const result = await runtime.respond(turn);
  assert.equal(result.text, "Qwen fallback.");
  assert.deepEqual(requests, ["http://gemma.test/api/chat", "http://qwen.test/v1/chat/completions"]);
  assert.equal(runtime.getTelemetry().effective_profile, QWEN_PROFILE_ID);
});

test("Gemma health check recognizes the exact Hugging Face Ollama model", async () => {
  const profiles = gemmaProfiles();
  const runtime = createAssistantRuntime({ enabled: true, profiles, preferredProfile: "gemma", logger: quietLogger,
    fetchImpl: async (url) => {
      assert.equal(url, "http://gemma.test/api/tags");
      return jsonResponse({ models: [{ name: profiles[GEMMA_PROFILE_ID].model }] });
    } });
  const telemetry = await runtime.refreshHealth();
  assert.equal(telemetry.online, "online");
});
