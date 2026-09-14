import assert from "node:assert/strict";
import test from "node:test";
import { executeAssistantToolCalls, lfmToolDefinitions, parseLfmToolCalls, validateToolCall } from "../src/assistant-tools.js";

const tools = [{ name: "weather", description: "Read weather", parameters: { type: "object", additionalProperties: false,
  properties: { city: { type: "string" }, days: { type: "integer" } }, required: ["city"] },
  annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: true }, execute: async (args) => ({ ...args, rain: true }) }];

test("native LFM tool envelopes parse parallel typed calls without eval", () => {
  const parsed = parseLfmToolCalls("<think>hidden</think><|tool_call_start|>[weather(city='Paris', days=2), weather(city=\"Bangkok\", days=1)]<|tool_call_end|>");
  assert.equal(parsed.protocol, "lfm_native");
  assert.deepEqual(parsed.calls, [
    { name: "weather", arguments: { city: "Paris", days: 2 } },
    { name: "weather", arguments: { city: "Bangkok", days: 1 } },
  ]);
});

test("native LFM parser accepts bounded nested JSON argument values", () => {
  const parsed = parseLfmToolCalls('<|tool_call_start|>[weather(city="Paris", days=2, meta={"units":["c", "f"]})]<|tool_call_end|>');
  assert.deepEqual(parsed.calls[0].arguments.meta, { units: ["c", "f"] });
});

test("tool schema rejects missing, unknown, and incorrectly typed arguments", () => {
  assert.equal(validateToolCall({ name: "weather", arguments: {} }, tools).error, "missing_required_argument");
  assert.equal(validateToolCall({ name: "weather", arguments: { city: "Paris", extra: 1 } }, tools).error, "unknown_argument");
  assert.equal(validateToolCall({ name: "weather", arguments: { city: "Paris", days: "two" } }, tools).error, "invalid_argument_type");
});

test("destructive tools require independent host authorization before execution", async () => {
  let executed = 0;
  const destructive = [{ name: "erase", parameters: { type: "object", properties: { target: { type: "string" } }, required: ["target"] },
    annotations: { readOnlyHint: false, destructiveHint: true }, execute: async () => { executed += 1; } }];
  const denied = await executeAssistantToolCalls([{ name: "erase", arguments: { target: "x" } }], { tools: destructive });
  assert.equal(denied[0].error, "host_authorization_required"); assert.equal(executed, 0);
  const allowed = await executeAssistantToolCalls([{ name: "erase", arguments: { target: "x" } }], { tools: destructive, hostAuthorize: async () => true });
  assert.equal(allowed[0].ok, true); assert.equal(executed, 1);
});

test("tool definitions exclude executable functions and malformed envelopes fail closed", () => {
  assert.doesNotMatch(lfmToolDefinitions(tools), /execute/);
  assert.throws(() => parseLfmToolCalls("<|tool_call_start|>[weather(city='Paris')]"), /protocol_invalid/);
});
