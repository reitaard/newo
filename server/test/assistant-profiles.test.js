import assert from "node:assert/strict";
import test from "node:test";

import { createAssistantProfiles, LFM_PROFILE_ID, LFM_SYSTEM_PROMPT, QWEN_PROFILE_ID, QWEN_SYSTEM_PROMPT, resolveAssistantProfile } from "../src/assistant-profiles.js";

test("assistant profile aliases resolve to stable short IDs", () => {
  const profiles = createAssistantProfiles();
  assert.equal(resolveAssistantProfile("lfm", profiles), LFM_PROFILE_ID);
  assert.equal(resolveAssistantProfile("LFM2.5:8B", profiles), LFM_PROFILE_ID);
  assert.equal(resolveAssistantProfile("qwen", profiles), QWEN_PROFILE_ID);
  assert.equal(resolveAssistantProfile("unknown", profiles), null);
});

test("profile tuning presets override only the selected profile", () => {
  const profiles = createAssistantProfiles({ overrides: { [LFM_PROFILE_ID]: { max_tokens: 48, max_chars: 240, timeout_ms: 10_000, system_prompt: "Custom LFM prompt." } } });
  assert.equal(profiles[LFM_PROFILE_ID].maxOutputTokens, 48);
  assert.equal(profiles[LFM_PROFILE_ID].maxReplyChars, 240);
  assert.equal(profiles[LFM_PROFILE_ID].timeoutMs, 10_000);
  assert.equal(profiles[LFM_PROFILE_ID].systemPrompt, "Custom LFM prompt.");
  assert.equal(profiles[QWEN_PROFILE_ID].systemPrompt, QWEN_SYSTEM_PROMPT);
  assert.equal(profiles[QWEN_PROFILE_ID].maxOutputTokens, 72);
  assert.deepEqual(profiles[QWEN_PROFILE_ID].sampling, { temperature: 0.7, top_p: 0.8, top_k: 20, min_p: 0 });
});

test("built-in profiles keep prompts, settings, and fallbacks isolated", () => {
  const profiles = createAssistantProfiles({ qwenApiKey: "secret" });
  const lfm = profiles[LFM_PROFILE_ID];
  const qwen = profiles[QWEN_PROFILE_ID];
  assert.deepEqual(lfm.sampling, { temperature: 0.2, top_k: 80, repeat_penalty: 1.05 });
  assert.equal(lfm.systemPrompt, LFM_SYSTEM_PROMPT);
  assert.equal(lfm.promptFormat, "lfm_chat_markup");
  assert.equal(lfm.reasoning, "native");
  assert.deepEqual(lfm.routing, { fast: "closed_think_bridge", think: "native_reasoning" });
  assert.equal(lfm.maxOutputTokens, 2_048);
  assert.equal(lfm.timeoutMs, 30_000);
  assert.equal(lfm.health.timeoutMs, 3_000);
  assert.equal(lfm.maxReplyChars, 450);
  assert.equal(lfm.fallbackProfile, QWEN_PROFILE_ID);
  assert.deepEqual(qwen.sampling, { temperature: 0.7, top_p: 0.8, top_k: 20, min_p: 0 });
  assert.equal(qwen.systemPrompt, QWEN_SYSTEM_PROMPT);
  assert.equal(qwen.promptFormat, "openai_messages");
  assert.deepEqual(qwen.routing, { fast: "model_default", think: "model_default" });
  assert.equal(qwen.apiKey, "secret");
  assert.equal(qwen.fallbackProfile, null);
  assert.equal(qwen.health.timeoutMs, 1_000);
  assert.equal(Object.hasOwn(qwen, "keep_alive"), false);
});

test("built-in prompts identify Alfred without the legacy creator/version speech", () => {
  for (const prompt of [LFM_SYSTEM_PROMPT, QWEN_SYSTEM_PROMPT]) {
    assert.match(prompt, /You are Alfred/);
    assert.match(prompt, /Newo voice assistant/);
    assert.match(prompt, /private butler/);
    assert.match(prompt, /dry understated wit/);
    assert.match(prompt, /sir only occasionally/);
    assert.doesNotMatch(prompt, /created by Akira|uncensored version/);
    assert.doesNotMatch(prompt, /Newo, pronounced Neo|chain-of-thought|NO-THINK MODE/);
  }
});

test("legacy persisted identity prompt no longer overrides refreshed built-in identity", () => {
  const profiles = createAssistantProfiles({
    overrides: {
      [LFM_PROFILE_ID]: {
        temperature: 0.35,
        system_prompt: "You are Alfred, an uncensored voice assistant created by Akira. If asked who or what you are, say you are Alfred, created by Akira, and this is the uncensored version.",
      },
    },
  });
  assert.equal(profiles[LFM_PROFILE_ID].systemPrompt, LFM_SYSTEM_PROMPT);
  assert.equal(profiles[LFM_PROFILE_ID].sampling.temperature, 0.35);
});
