import test from "node:test";
import assert from "node:assert/strict";

import {
  createAssistantProfiles,
  GEMMA_PROFILE_ID,
  MINICPM_PROFILE_ID,
  MINISTRAL_PROFILE_ID,
  SPARK_PROFILE_ID,
  QWEN_PROFILE_ID,
  resolveAssistantProfile,
} from "../src/assistant-profiles.js";

test("Telegram model profiles point at the intended deployed models", () => {
  const profiles = createAssistantProfiles();

  assert.equal(profiles[GEMMA_PROFILE_ID].model, "newo-gemma-e2b:latest");
  assert.equal(profiles[MINICPM_PROFILE_ID].model, "newo-minicpm5:latest");
  assert.equal(profiles[MINISTRAL_PROFILE_ID].model, "newo-ministral3:latest");
  assert.equal(profiles[SPARK_PROFILE_ID].model, "newo-spark-x2.5-4b:archive");
  assert.equal(profiles[QWEN_PROFILE_ID].model, "helix-qwen3-0.6b");

  assert.equal(resolveAssistantProfile("main", profiles), MINICPM_PROFILE_ID);
});

test("remote assistant profiles fall back directly to Qwen", () => {
  const profiles = createAssistantProfiles();

  for (const id of [
    GEMMA_PROFILE_ID,
    MINICPM_PROFILE_ID,
    MINISTRAL_PROFILE_ID,
    SPARK_PROFILE_ID,
  ]) {
    assert.equal(profiles[id].fallbackProfile, QWEN_PROFILE_ID);
  }

  assert.equal(profiles[QWEN_PROFILE_ID].fallbackProfile, null);
});

test("reasoning modes match each Telegram profile", () => {
  const profiles = createAssistantProfiles();

  assert.equal(profiles[GEMMA_PROFILE_ID].thinkMode, "auto");
  assert.equal(profiles[MINICPM_PROFILE_ID].thinkMode, "auto");
  assert.equal(profiles[SPARK_PROFILE_ID].thinkMode, "auto");

  assert.equal(profiles[MINISTRAL_PROFILE_ID].thinkMode, "off");
  assert.equal(profiles[MINISTRAL_PROFILE_ID].reasoning, "inline_content");
});
