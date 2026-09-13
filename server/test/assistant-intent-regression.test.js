import assert from "node:assert/strict";
import test from "node:test";

import { createAssistantRuntime, directClockShortcut } from "../src/assistant.js";
import { createAssistantTurnRuntime } from "../src/assistant-turn.js";

const quietLogger = { info() {}, warn() {} };

function jsonResponse(payload) {
  return new Response(JSON.stringify(payload), { status: 200, headers: { "content-type": "application/json" } });
}

test("natural local-time phrasings route deterministically", async () => {
  const now = new Date("2026-09-13T11:05:42.000Z");
  const zone = "Asia/Phnom_Penh";
  for (const text of [
    "Can you tell me the time right now?",
    "Tell me what time it is",
    "What time is it Neo?",
    "What time is it Neil?",
  ]) {
    assert.equal(directClockShortcut(text, now, zone)?.text, "It's six oh five PM.");
  }
  assert.equal(directClockShortcut("What time is it in Tokyo?", now, zone), null);

  let fetchCalls = 0;
  const runtime = createAssistantRuntime({
    enabled: true,
    baseUrl: "http://local",
    model: "model",
    timeZone: zone,
    now: () => now,
    logger: quietLogger,
    fetchImpl: async () => { fetchCalls += 1; throw new Error("LLM must not be called"); },
  });
  const result = await runtime.respond({
    deviceId: "newo-01",
    streamId: "time-natural",
    text: "Can you tell me the time right now?",
  });
  assert.equal(result.timings.route, "local_time");
  assert.equal(result.timings.time_context, true);
  assert.equal(result.timings.llm_request_ms, 0);
  assert.equal(fetchCalls, 0);
});

test("combined previous-question requests include the latest exchange", async () => {
  const requests = [];
  let call = 0;
  const runtime = createAssistantRuntime({
    enabled: true,
    baseUrl: "http://local",
    model: "model",
    logger: quietLogger,
    fetchImpl: async (_url, options) => {
      requests.push(JSON.parse(options.body));
      call += 1;
      return jsonResponse({ choices: [{ message: { content: call === 1 ? "Bermuda answer." : "Pyramid answer plus memory." } }] });
    },
  });

  await runtime.respond({
    deviceId: "newo-01",
    streamId: "first",
    text: "Tell me something about Bermuda Triangle",
  });
  const result = await runtime.respond({
    deviceId: "newo-01",
    streamId: "second",
    text: "Tell me three facts about the Pyramid of Giza and what did I ask you last time",
  });

  assert.equal(result.timings.history_available, 1);
  assert.equal(result.timings.history_used, 1);
  assert.deepEqual(requests[1].messages.map((message) => message.role), ["system", "user", "assistant", "user"]);
  assert.equal(requests[1].messages[1].content, "Tell me something about Bermuda Triangle");
  assert.equal(requests[1].messages[2].content, "Bermuda answer.");
});

test("assistant turn preserves a profile-bounded reply longer than the legacy turn cap", async () => {
  const text = "A".repeat(420);
  let spoken = null;
  let options = null;
  const assistant = {
    async respond() { return { kind: "response", text, timings: { llm_request_ms: 1 } }; },
    abortDevice() {}, close() {},
  };
  const speakerRuntime = {
    speak(value, supplied) {
      spoken = value;
      options = supplied;
      return { kind: "queued", playbackId: "playback", completion: Promise.resolve({ kind: "complete" }) };
    },
  };
  const turns = createAssistantTurnRuntime({
    assistant, speakerRuntime, isPersistentSpeakerEnabled: () => true,
    maxReplyChars: 300, logger: quietLogger,
  });

  const started = turns.handleFinalTranscript({ deviceId: "newo-01", streamId: "long-reply", text: "summary" });
  assert.equal((await started.completion).kind, "complete");
  assert.equal(spoken, text);
  assert.equal(options.maxChars, 420);
});
