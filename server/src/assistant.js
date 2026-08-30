const SYSTEM_PROMPT = [
  "You are Newo, a concise conversational voice assistant.",
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
  const endpoint = baseUrl ? `${String(baseUrl).replace(/\/+$/, "")}/v1/chat/completions` : null;

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
          messages: [{ role: "system", content: SYSTEM_PROMPT }, { role: "user", content: transcript }],
          max_tokens: maxOutputTokens, temperature: 0.45, reasoning_effort: "none", stream: false,
        }),
      });
      if (!response.ok) throw assistantError("assistant_http_error", String(response.status));
      let payload;
      try { payload = await response.json(); }
      catch { throw assistantError("assistant_invalid_response"); }
      const answer = boundedText(payload?.choices?.[0]?.message?.content, maxReplyChars);
      if (!answer) return { kind: "empty" };
      const completedAt = performance.now();
      const timings = { llm_request_ms: Math.round(completedAt - startedAt) };
      logger?.info({ device_id: deviceId, stream_id: streamId, reply_chars: answer.length, ...timings }, "Assistant text ready");
      return { kind: "response", text: answer, timings };
    } catch (error) {
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

  function abortDevice(deviceId) {
    active.get(deviceId)?.abort(assistantError("assistant_cancelled"));
  }
  function close() {
    closing = true;
    for (const controller of active.values()) controller.abort(assistantError("assistant_shutdown"));
  }

  return { respond, abortDevice, close, isActive: (deviceId) => active.has(deviceId) };
}
