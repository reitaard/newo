const WEB_TOOL_NAMES = Object.freeze(["web_search", "web_read"]);

export const CAPABILITY_TOOL_POLICY = Object.freeze({
  knowledge: Object.freeze([]),
  calculator: Object.freeze([]),
  "unit.convert": Object.freeze([]),
  "time.current": Object.freeze([]),
  "time.convert": Object.freeze([]),
  "currency.exchange": WEB_TOOL_NAMES,
  "weather.current": WEB_TOOL_NAMES,
  "weather.forecast": WEB_TOOL_NAMES,
  "sports.score": WEB_TOOL_NAMES,
  "sports.schedule": WEB_TOOL_NAMES,
  "market.quote": WEB_TOOL_NAMES,
  "web.search": Object.freeze(["web_search"]),
  "web.read": Object.freeze(["web_read"]),
  "memory.retrieve": Object.freeze(["memory_retrieve"]),
  "device.state": Object.freeze(["device_state"]),
  "device.control": Object.freeze(["device_control"]),
  "sensor.read": Object.freeze(["sensor_read"]),
  "camera.inspect": Object.freeze(["camera_inspect"]),
});

function routerError(code) {
  const error = new Error(code);
  error.code = code;
  return error;
}

export function toolsForCapability(decision, availableTools = []) {
  const available = new Set(availableTools.map((tool) => tool.name));
  // Disabled, unavailable, invalid, and trained abstention all preserve the
  // pre-router behavior: let LFM decide among the tools it already had.
  if (!decision || decision.fallback || decision.abstain) return availableTools;
  const names = CAPABILITY_TOOL_POLICY[decision.primary] ?? [];
  return availableTools.filter((tool) => names.includes(tool.name) && available.has(tool.name));
}

export function createCapabilityRouterClient({
  enabled = false,
  baseUrl = "http://127.0.0.1:8791",
  timeoutMs = 75,
  fetchImpl = fetch,
  logger = null,
} = {}) {
  const endpoint = String(baseUrl).replace(/\/+$/, "");
  let last = { status: enabled ? "unknown" : "disabled", model: null, at: null, error: null };

  async function request(path, options = {}, requestTimeoutMs = timeoutMs) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(routerError("capability_router_timeout")), requestTimeoutMs);
    try {
      const response = await fetchImpl(`${endpoint}${path}`, { ...options, signal: controller.signal });
      let payload = null;
      try { payload = await response.json(); } catch {}
      if (!response.ok) throw routerError(payload?.error?.code ?? "capability_router_unavailable");
      return payload;
    } catch (error) {
      if (controller.signal.aborted) throw controller.signal.reason;
      if (error?.code) throw error;
      throw routerError("capability_router_unavailable");
    } finally {
      clearTimeout(timer);
    }
  }

  async function classify(text) {
    if (!enabled) return { enabled: false, fallback: true, reason: "disabled", latency_ms: null };
    const startedAt = performance.now();
    try {
      const result = await request("/v1/route", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ text }),
      });
      const valid = (result.primary === null || typeof result.primary === "string") &&
        typeof result.raw_primary === "string" && typeof result.abstain === "boolean" &&
        typeof result.confidence === "number" && typeof result.margin === "number" &&
        typeof result.source_need === "string" && result.scores && typeof result.scores === "object";
      if (!valid) throw routerError("capability_router_invalid_response");
      const decision = { enabled: true, fallback: false, ...result,
        request_latency_ms: Math.round(performance.now() - startedAt) };
      logger?.info({ capability_primary: decision.primary, capability_raw_primary: decision.raw_primary,
        capability_abstain: decision.abstain, capability_confidence: decision.confidence,
        capability_margin: decision.margin, capability_source_need: decision.source_need,
        capability_router_latency_ms: decision.latency_ms,
        capability_router_request_ms: decision.request_latency_ms }, "Assistant capability routed");
      last = { status: "ready", model: "setfit-minilm-router-v2", at: Date.now(), error: null };
      return decision;
    } catch (error) {
      const decision = { enabled: true, fallback: true, reason: error?.code ?? "capability_router_unavailable",
        latency_ms: null, request_latency_ms: Math.round(performance.now() - startedAt) };
      logger?.warn({ capability_router_fallback: true, capability_router_reason: decision.reason,
        capability_router_request_ms: decision.request_latency_ms }, "Assistant capability router failed open");
      last = { status: decision.reason === "model_missing" ? "model_missing" : "model_unavailable",
        model: "setfit-minilm-router-v2", at: Date.now(), error: decision.reason };
      return decision;
    }
  }

  async function health() {
    if (!enabled) return { status: "disabled", model: null };
    try {
      const result = await request("/healthz", {}, Math.max(timeoutMs, 1_000));
      last = { ...result, at: Date.now(), error: null };
      return result;
    } catch (error) {
      last = { status: error?.code === "model_missing" ? "model_missing" : "model_unavailable",
        model: "setfit-minilm-router-v2", at: Date.now(), error: error?.code };
      return { status: last.status, error: error?.code };
    }
  }

  return { enabled, classify, health, getTelemetry: () => ({ ...last, enabled }) };
}
