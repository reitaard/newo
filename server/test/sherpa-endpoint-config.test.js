import assert from "node:assert/strict";
import test from "node:test";

import { DEFAULT_SHERPA_ENDPOINTS, resolveSherpaEndpointConfig } from "../src/sherpa-endpoint-config.js";

test("Sherpa endpoint defaults stay at the tuned values", () => {
  assert.deepEqual(resolveSherpaEndpointConfig({}), DEFAULT_SHERPA_ENDPOINTS);
});

test("Sherpa endpoint caller defaults remain valid without env overrides", () => {
  assert.deepEqual(resolveSherpaEndpointConfig({}, {
    rule1Seconds: 2.5,
    rule2Seconds: 1.25,
    rule3Seconds: 30,
  }), {
    rule1Seconds: 2.5,
    rule2Seconds: 1.25,
    rule3Seconds: 30,
  });
});

test("Sherpa endpoint environment overrides are applied exactly", () => {
  assert.deepEqual(resolveSherpaEndpointConfig({
    VOICE_ASR_ENDPOINT_RULE1_S: "1.75",
    VOICE_ASR_ENDPOINT_RULE2_S: "0.8",
    VOICE_ASR_ENDPOINT_RULE3_S: "24",
  }), {
    rule1Seconds: 1.75,
    rule2Seconds: 0.8,
    rule3Seconds: 24,
  });
});

test("invalid Sherpa endpoint overrides fail loudly instead of silently falling back", () => {
  assert.throws(
    () => resolveSherpaEndpointConfig({ VOICE_ASR_ENDPOINT_RULE2_S: "0" }),
    /VOICE_ASR_ENDPOINT_RULE2_S/,
  );
  assert.throws(
    () => resolveSherpaEndpointConfig({ VOICE_ASR_ENDPOINT_RULE3_S: "not-a-number" }),
    /VOICE_ASR_ENDPOINT_RULE3_S/,
  );
});
