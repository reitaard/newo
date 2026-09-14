import { readFileSync } from "node:fs";

const DEFAULT_BASE_URL = "http://127.0.0.1:8790";

function toolError(code, message = code) {
  const error = new Error(message);
  error.code = code;
  return error;
}

export const WEB_TOOL_DEFINITIONS = Object.freeze([
  Object.freeze({
    name: "web_search",
    description: "Search current web sources. Use for explicit searches and facts that may have changed; do not use for stable knowledge you already know.",
    parameters: Object.freeze({
      type: "object",
      properties: Object.freeze({
        query: Object.freeze({ type: "string", description: "Focused search query" }),
        max_results: Object.freeze({ type: "integer", description: "Number of results from 1 to 5" }),
      }),
      required: Object.freeze(["query"]),
      additionalProperties: false,
    }),
    annotations: Object.freeze({ readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: true }),
  }),
  Object.freeze({
    name: "web_read",
    description: "Read one source URL when its full content is needed. Prefer URLs returned by web_search.",
    parameters: Object.freeze({
      type: "object",
      properties: Object.freeze({
        url: Object.freeze({ type: "string", description: "HTTP or HTTPS source URL" }),
        max_chars: Object.freeze({ type: "integer", description: "Maximum content characters from 1000 to 12000" }),
      }),
      required: Object.freeze(["url"]),
      additionalProperties: false,
    }),
    annotations: Object.freeze({ readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: true }),
  }),
]);

export function readAgentToolsToken({ token, envFile } = {}) {
  if (token) return String(token).trim();
  if (!envFile) return "";
  let content;
  try { content = readFileSync(envFile, "utf8"); } catch (error) {
    if (error?.code === "ENOENT") return "";
    throw error;
  }
  const line = content.split(/\r?\n/).find((entry) => entry.startsWith("AGENT_TOOLS_TOKEN="));
  return line?.slice("AGENT_TOOLS_TOKEN=".length).trim() ?? "";
}

export function createAssistantWebTools({
  enabled = false,
  baseUrl = DEFAULT_BASE_URL,
  token = "",
  timeoutMs = 12_000,
  fetchImpl = fetch,
  logger = null,
} = {}) {
  const normalizedBase = String(baseUrl).replace(/\/+$/, "");
  const available = Boolean(enabled && token);

  async function invoke(name, rawArgs, { signal } = {}) {
    if (!available) throw toolError("web_tools_unavailable");
    const args = { ...rawArgs };
    let path;
    if (name === "web_search") {
      path = "/v1/tools/web/search";
      args.max_results ??= 5;
      if (args.max_results < 1 || args.max_results > 5) throw toolError("invalid_tool_arguments");
    } else if (name === "web_read") {
      path = "/v1/tools/web/read";
      args.max_chars ??= 8_000;
      if (args.max_chars < 1_000 || args.max_chars > 12_000) throw toolError("invalid_tool_arguments");
    } else throw toolError("unknown_tool");

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(toolError("agent_tools_timeout")), timeoutMs);
    const abort = () => controller.abort(signal.reason ?? toolError("assistant_cancelled"));
    signal?.addEventListener("abort", abort, { once: true });
    const startedAt = performance.now();
    try {
      const response = await fetchImpl(`${normalizedBase}${path}`, {
        method: "POST",
        headers: { authorization: `Bearer ${token}`, "content-type": "application/json" },
        body: JSON.stringify(args),
        signal: controller.signal,
      });
      let payload = null;
      try { payload = await response.json(); } catch {}
      if (!response.ok) {
        const code = payload?.error?.code;
        throw toolError(code === "provider_timeout" ? "agent_tools_timeout" : "agent_tools_error");
      }
      const elapsedMs = Math.round(performance.now() - startedAt);
      logger?.info({ tool_name: name, agent_tools_latency_ms: elapsedMs,
        provider_latency_ms: Number.isFinite(payload?.provider_elapsed_ms) ? payload.provider_elapsed_ms : null,
        result_count: payload?.result_count ?? (payload?.content ? 1 : 0) }, "Assistant web tool completed");
      return { value: payload, elapsedMs, providerElapsedMs: Number.isFinite(payload?.provider_elapsed_ms) ? payload.provider_elapsed_ms : null };
    } catch (error) {
      if (controller.signal.aborted) throw controller.signal.reason ?? toolError("agent_tools_timeout");
      if (error?.code) throw error;
      throw toolError("agent_tools_error");
    } finally {
      clearTimeout(timer);
      signal?.removeEventListener("abort", abort);
    }
  }

  return { available, definitions: WEB_TOOL_DEFINITIONS, invoke };
}
