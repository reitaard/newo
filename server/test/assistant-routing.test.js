import assert from "node:assert/strict";
import test from "node:test";
import { authorizeToolInvocation, classifyAssistantActivity, progressFeedbackFor, routeAssistantRequest } from "../src/assistant-routing.js";

test("router keeps clear low-risk requests FAST and classifies activity", () => {
  assert.deepEqual(routeAssistantRequest({ text: "Who wrote Frankenstein?" }),
    { route: "FAST", reasons: ["low_risk_single_step"], activity: "conversation" });
  assert.equal(classifyAssistantActivity("Calculate 17 percent of 80"), "calculation");
});

test("router biases structural uncertainty and tool planning toward THINK", () => {
  assert.equal(routeAssistantRequest({ text: "Sensor A says safe, but sensor B says unsafe." }).route, "THINK");
  assert.equal(routeAssistantRequest({ text: "do it", context: { hasHistory: false } }).route, "THINK");
  assert.equal(routeAssistantRequest({ text: "Handle this", candidateTools: [
    { name: "read", annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false } },
    { name: "write", missingRequired: ["target"], dependsOnPrevious: true },
  ] }).route, "THINK");
});

test("MCP annotations influence routing but never authorize destructive work", () => {
  const read = { annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false } };
  const destructive = { annotations: { readOnlyHint: false, destructiveHint: true, idempotentHint: true } };
  assert.equal(authorizeToolInvocation({ tool: read }).allowed, true);
  assert.deepEqual(authorizeToolInvocation({ tool: destructive }),
    { allowed: false, reason: "host_authorization_required", hintsTrustedAsAuthorization: false });
  assert.equal(authorizeToolInvocation({ tool: destructive, hostAuthorized: true }).allowed, true);
});

test("progress feedback is delayed-policy content only for non-conversational THINK work", () => {
  assert.equal(progressFeedbackFor({ route: "FAST", activity: "search" }), null);
  assert.equal(progressFeedbackFor({ route: "THINK", activity: "conversation" }), null);
  assert.match(progressFeedbackFor({ route: "THINK", activity: "sensor_fusion" }), /sensor/i);
  assert.notEqual(progressFeedbackFor({ route: "THINK", activity: "search", variation: 0 }),
    progressFeedbackFor({ route: "THINK", activity: "search", variation: 1 }));
});
