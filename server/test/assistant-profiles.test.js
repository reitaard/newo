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

test("built-in profiles keep prompts, settings, and fallbacks isolated", () => {
  const profiles = createAssistantProfiles({ qwenApiKey: "secret" });
  const lfm = profiles[LFM_PROFILE_ID];
  const qwen = profiles[QWEN_PROFILE_ID];
  assert.deepEqual(lfm.sampling, { temperature: 0.2, top_k: 80, repeat_penalty: 1.05 });
  assert.equal(lfm.systemPrompt, LFM_SYSTEM_PROMPT);
  assert.equal(lfm.promptFormat, "lfm_chat_markup");
  assert.equal(lfm.maxOutputTokens, 64);
  assert.equal(lfm.fallbackProfile, QWEN_PROFILE_ID);
  assert.deepEqual(qwen.sampling, { temperature: 0.7, top_p: 0.8, top_k: 20, min_p: 0 });
  assert.equal(qwen.systemPrompt, QWEN_SYSTEM_PROMPT);
  assert.equal(qwen.promptFormat, "openai_messages");
  assert.equal(qwen.apiKey, "secret");
  assert.equal(qwen.fallbackProfile, null);
  assert.equal(Object.hasOwn(qwen, "keep_alive"), false);
});
