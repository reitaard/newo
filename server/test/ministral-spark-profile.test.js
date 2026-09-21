import test from "node:test";
import assert from "node:assert/strict";

import {
  createAssistantProfiles,
  MINISTRAL_PROFILE_ID,
  SPARK_PROFILE_ID,
  QWEN_PROFILE_ID,
  resolveAssistantProfile,
} from "../src/assistant-profiles.js";

test("Ministral profile is permanently no-think", () => {
  const profiles = createAssistantProfiles();
  const p = profiles[MINISTRAL_PROFILE_ID];

  assert.equal(MINISTRAL_PROFILE_ID, "ministral3:3b");
  assert.equal(resolveAssistantProfile("ministral", profiles), MINISTRAL_PROFILE_ID);
  assert.equal(p.model, "newo-ministral3:latest");
  assert.equal(p.provider, "ollama_chat");

  assert.equal(p.thinkMode, "off");
  assert.equal(p.routing.fast, "off");
  assert.equal(p.routing.think, "off");

  assert.ok(p.sampling.stop.includes("<|im_end|>"));
  assert.equal(p.fallbackProfile, QWEN_PROFILE_ID);
});

test("Spark profile supports FAST and THINK routing", () => {
  const profiles = createAssistantProfiles();
  const p = profiles[SPARK_PROFILE_ID];

  assert.equal(SPARK_PROFILE_ID, "spark-x2.5:4b");
  assert.equal(resolveAssistantProfile("spark", profiles), SPARK_PROFILE_ID);
  assert.equal(resolveAssistantProfile("spark2.5", profiles), SPARK_PROFILE_ID);

  assert.equal(p.model, "newo-spark-x2.5:latest");
  assert.equal(p.provider, "ollama_chat");

  assert.equal(p.thinkMode, "auto");
  assert.equal(p.routing.fast, "off");
  assert.equal(p.routing.think, "on");

  assert.equal(p.fallbackProfile, QWEN_PROFILE_ID);
});
