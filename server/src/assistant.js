export const ASSISTANT_SYSTEM_PROMPT = [
  "You are Newo, pronounced Neo, a concise conversational voice assistant.",
  "In normal conversation, refer to your name naturally as Neo; mention the Newo spelling or branding only when asked.",
  "Reply in plain, natural language for speech, usually one to three short sentences.",
  "Do not use markdown, reveal reasoning, claim actions or tools you do not have, or continue on your own.",
  "If you cannot do something, say so briefly.",
].join(" ");

function boundedText(value, maxChars) {
  const text = String(value ?? "")
    .replace(/<think>[\s\S]*?<\/think>/gi, "")
    .replace(/\s+/g, " ")
    .trim();
  if (!text) return "";
  if (text.length <= maxChars) return text;
  const clipped = text.slice(0, maxChars + 1);
  const boundary = clipped.lastIndexOf(" ");
  return `${clipped.slice(boundary >= Math.floor(maxChars * 0.7) ? boundary : maxChars).replace(/[\s,;:]+$/, "")}…`;
}

function assistantError(code, detail) {
  const error = new Error(detail ? `${code}: ${detail}` : code);
  error.code = code;
  return error;
}

/** A bounded, provider-neutral OpenAI-chat client for one finalized voice turn. */
export function createAssistantRuntime({
  enabled = false, baseUrl, model, apiKey, timeoutMs = 15_000, maxOutputTokens = 48,
  maxReplyChars = 300, fetchImpl = fetch, logger = null,
} = {}) {
  const active = new Map();
  let closing = false;
  let qwenState = enabled ? "unknown" : "disabled";
  const base = baseUrl ? String(baseUrl).replace(/\/+$/, "") : null;
  const endpoint = base ? `${base}/v1/chat/completions` : null;
  const modelsEndpoint = base ? `${base}/v1/models` : null;

  function getTelemetry() {
    return { enabled, model: model ?? null, qwen: qwenState, active: active.size > 0 };
  }

  async function refreshHealth() {
    if (!enabled) return getTelemetry();
    if (!modelsEndpoint || !model) { qwenState = "offline"; return getTelemetry(); }
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), Math.min(timeoutMs, 1_000));
    timer.unref();
    try {
      const headers = apiKey ? { authorization: `Bearer ${apiKey}` } : undefined;
      const response = await fetchImpl(modelsEndpoint, { headers, signal: controller.signal });
      const payload = response.ok ? await response.json() : null;
      const models = payload?.data;
      qwenState = Array.isArray(models) && models.some((item) => item?.id === model) ? "online" : "offline";
    } catch {
      qwenState = "offline";
    } finally {
      clearTimeout(timer);
    }
    return getTelemetry();
  }

  async function respond({ deviceId, streamId, text }) {
    const transcript = boundedText(text, 800);
    if (!enabled || closing) return { kind: "disabled" };
    if (!transcript) return { kind: "empty" };
    if (!endpoint || !model) return { kind: "unavailable" };
    if (active.has(deviceId)) return { kind: "busy" };

    const controller = new AbortController();
    const startedAt = performance.now();
    const timer = setTimeout(() => controller.abort(assistantError("assistant_timeout")), timeoutMs);
    timer.unref();
    active.set(deviceId, controller);
    try {
      logger?.info({ device_id: deviceId, stream_id: streamId, transcript_chars: transcript.length }, "Assistant LLM request started");
      const headers = { "content-type": "application/json" };
      if (apiKey) headers.authorization = `Bearer ${apiKey}`;
      const response = await fetchImpl(endpoint, {
        method: "POST", headers, signal: controller.signal,
        body: JSON.stringify({
          model,
          messages: [{ role: "system", content: ASSISTANT_SYSTEM_PROMPT }, { role: "user", content: transcript }],
          max_tokens: maxOutputTokens, temperature: 0.45, reasoning_effort: "none", stream: false,
        }),
      });
      if (!response.ok) throw assistantError("assistant_http_error", String(response.status));
      let payload;
      try { payload = await response.json(); }
      catch { throw assistantError("assistant_invalid_response"); }
      const answer = boundedText(payload?.choices?.[0]?.message?.content, maxReplyChars);
      if (!answer) return { kind: "empty" };
      qwenState = "online";
      const completedAt = performance.now();
      const timings = { llm_request_ms: Math.round(completedAt - startedAt) };
      logger?.info({ device_id: deviceId, stream_id: streamId, reply_chars: answer.length, ...timings }, "Assistant text ready");
      return { kind: "response", text: answer, timings };
    } catch (error) {
      qwenState = "offline";
      const code = controller.signal.aborted
        ? controller.signal.reason?.code ?? "assistant_cancelled"
        : error?.code ?? "assistant_request_failed";
      logger?.warn({ device_id: deviceId, stream_id: streamId, error_code: code }, "Assistant LLM request failed");
      return { kind: code === "assistant_timeout" ? "timeout" : "error", error: code };
    } finally {
      clearTimeout(timer);
      if (active.get(deviceId) === controller) active.delete(deviceId);
    }
  }

  function abortDevice(deviceId) { active.get(deviceId)?.abort(assistantError("assistant_cancelled")); }
  function close() {
    closing = true;
    for (const controller of active.values()) controller.abort(assistantError("assistant_shutdown"));
  }

  return { respond, refreshHealth, getTelemetry, abortDevice, close, isActive: (deviceId) => active.has(deviceId) };
}
