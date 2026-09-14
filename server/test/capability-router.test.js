import assert from "node:assert/strict";
import test from "node:test";
import { CAPABILITY_TOOL_POLICY, createCapabilityRouterClient, toolsForCapability } from "../src/capability-router.js";

const available = [
  { name: "web_search" }, { name: "web_read" }, { name: "memory_retrieve" },
  { name: "device_state" }, { name: "device_control" }, { name: "sensor_read" },
  { name: "camera_inspect" },
];

test("capability mapping exposes only centralized relevant available tools", () => {
  assert.deepEqual(toolsForCapability({ primary: "knowledge", abstain: false }, available), []);
  assert.deepEqual(toolsForCapability({ primary: "web.search", abstain: false }, available).map((tool) => tool.name), ["web_search"]);
  assert.deepEqual(toolsForCapability({ primary: "web.read", abstain: false }, available).map((tool) => tool.name), ["web_read"]);
  assert.deepEqual(toolsForCapability({ primary: "weather.current", abstain: false }, available).map((tool) => tool.name), ["web_search", "web_read"]);
  assert.deepEqual(toolsForCapability({ primary: "currency.exchange", abstain: false }, available).map((tool) => tool.name), ["web_search", "web_read"]);
  assert.deepEqual(toolsForCapability({ primary: "sports.schedule", abstain: false }, available).map((tool) => tool.name), ["web_search", "web_read"]);
  assert.deepEqual(toolsForCapability({ primary: "market.quote", abstain: false }, available).map((tool) => tool.name), ["web_search", "web_read"]);
  assert.deepEqual(toolsForCapability({ primary: "memory.retrieve", abstain: false }, available).map((tool) => tool.name), ["memory_retrieve"]);
  assert.deepEqual(toolsForCapability({ primary: "device.state", abstain: false }, available).map((tool) => tool.name), ["device_state"]);
  assert.deepEqual(toolsForCapability({ primary: "device.control", abstain: false }, available).map((tool) => tool.name), ["device_control"]);
  assert.deepEqual(toolsForCapability({ primary: "sensor.read", abstain: false }, available).map((tool) => tool.name), ["sensor_read"]);
  assert.deepEqual(toolsForCapability({ primary: "camera.inspect", abstain: false }, available).map((tool) => tool.name), ["camera_inspect"]);
  assert.equal(CAPABILITY_TOOL_POLICY["device.control"].includes("web_search"), false);
});

test("trained abstain and router fallback preserve prior tool exposure", () => {
  assert.equal(toolsForCapability({ primary: null, abstain: true }, available).length, available.length);
  assert.equal(toolsForCapability({ fallback: true }, available).length, available.length);
});

test("client performs one validated inference and preserves v2 diagnostics", async () => {
  let calls = 0;
  const client = createCapabilityRouterClient({ enabled: true, fetchImpl: async (_url, options) => {
    calls += 1;
    assert.deepEqual(JSON.parse(options.body), { text: "latest release" });
    return new Response(JSON.stringify({ primary: "web.search", raw_primary: "web.search", capabilities: ["web.search"],
      scores: { "web.search": 0.8, knowledge: 0.2 }, confidence: 0.8, margin: 0.6,
      source_need: "live", abstain: false, latency_ms: 5.1 }), { status: 200 });
  } });
  const result = await client.classify("latest release");
  assert.equal(calls, 1);
  assert.equal(result.primary, "web.search");
  assert.equal(result.confidence, 0.8);
  assert.equal(result.margin, 0.6);
  assert.equal(result.fallback, false);
});

test("timeout and unavailable router fail open with structured state", async () => {
  const timeout = createCapabilityRouterClient({ enabled: true, timeoutMs: 5, fetchImpl: async (_url, options) =>
    new Promise((_resolve, reject) => options.signal.addEventListener("abort", () => reject(options.signal.reason), { once: true })) });
  const timed = await timeout.classify("hello");
  assert.equal(timed.fallback, true);
  assert.equal(timed.reason, "capability_router_timeout");

  const unavailable = createCapabilityRouterClient({ enabled: true, fetchImpl: async () => { throw new Error("offline"); } });
  const failed = await unavailable.classify("hello");
  assert.equal(failed.fallback, true);
  assert.equal(failed.reason, "capability_router_unavailable");
  assert.equal(unavailable.getTelemetry().status, "model_unavailable");
});

test("health distinguishes missing model from unavailable service", async () => {
  const missing = createCapabilityRouterClient({ enabled: true, fetchImpl: async () =>
    new Response(JSON.stringify({ error: { code: "model_missing" } }), { status: 503 }) });
  assert.deepEqual(await missing.health(), { status: "model_missing", error: "model_missing" });
});
