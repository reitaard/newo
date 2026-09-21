import test from "node:test";
import assert from "node:assert/strict";

import {
  createAssistantProfiles,
  MINICPM_PROFILE_ID,
  QWEN_PROFILE_ID,
  resolveAssistantProfile,
} from "../src/assistant-profiles.js";

test("MiniCPM5 profile uses native Ollama chat", () => {
  const profiles = createAssistantProfiles();
  const profile = profiles[MINICPM_PROFILE_ID];

  assert.equal(MINICPM_PROFILE_ID, "minicpm5:2b");
  assert.equal(resolveAssistantProfile("minicpm", profiles), MINICPM_PROFILE_ID);
  assert.equal(resolveAssistantProfile("minicpm5", profiles), MINICPM_PROFILE_ID);
  assert.equal(resolveAssistantProfile("main", profiles), MINICPM_PROFILE_ID);

  assert.equal(profile.provider, "ollama_chat");
  assert.equal(profile.endpoint, "/api/chat");
  assert.equal(profile.model, "newo-minicpm5:latest");

  assert.equal(profile.thinkMode, "auto");
  assert.equal(profile.routing.fast, "off");
  assert.equal(profile.routing.think, "on");

  assert.equal(profile.fallbackProfile, QWEN_PROFILE_ID);
});
