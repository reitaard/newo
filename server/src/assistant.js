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
    .replace(/<think>[\s\S]*$/gi, "")
    .replace(/<\/?think>/gi, "")
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

function rawLfmPrompt(transcript) {
  return `<|im_start|>system\n${ASSISTANT_SYSTEM_PROMPT}<|im_end|>\n<|im_start|>user\n${transcript}<|im_end|>\n<|im_start|>assistant\n<think>\nNo unnecessary reasoning. Close thinking and answer immediately.\n</think>\n`;
}

async function readOllamaStream(response, onFirstToken) {
  if (!response.body) throw assistantError("assistant_invalid_response");
  const decoder = new TextDecoder();
  let pending = "";
  let answer = "";
  const consume = (line) => {
    if (!line.trim()) return;
    let payload;
    try { payload = JSON.parse(line); }
    catch { throw assistantError("assistant_invalid_response"); }
    if (payload.error) throw assistantError("assistant_request_failed", String(payload.error));
    if (typeof payload.response === "string" && payload.response.length > 0) {
      onFirstToken();
      answer += payload.response;
    }
  };
  for await (const chunk of response.body) {
    pending += decoder.decode(chunk, { stream: true });
    const lines = pending.split(/\r?\n/);
    pending = lines.pop() ?? "";
    for (const line of lines) consume(line);
  }
  pending += decoder.decode();
  if (pending.trim()) consume(pending);
  return answer;
}

/** A bounded provider-selectable client for one finalized voice turn. */
export function createAssistantRuntime({
  enabled = false, provider = "openai_chat", baseUrl, model, apiKey, timeoutMs = 15_000, maxOutputTokens = 48,
  maxReplyChars = 300, fetchImpl = fetch, logger = null,
} = {}) {
  const active = new Map();
  let closing = false;
  let qwenState = enabled ? "unknown" : "disabled";
  const base = baseUrl ? String(baseUrl).replace(/\/+$/, "") : null;
  const endpoint = base ? `${base}${provider === "ollama_raw" ? "/api/generate" : "/v1/chat/completions"}` : null;
  const modelsEndpoint = base ? `${base}${provider === "ollama_raw" ? "/api/tags" : "/v1/models"}` : null;

  function getTelemetry() {
    return { enabled, provider, model: model ?? null, qwen: qwenState, active: active.size > 0 };
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
      const models = provider === "ollama_raw" ? payload?.models : payload?.data;
      qwenState = Array.isArray(models) && models.some((item) => (provider === "ollama_raw" ? item?.name ?? item?.model : item?.id) === model) ? "online" : "offline";
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
      const requestBody = provider === "ollama_raw" ? {
        model, prompt: rawLfmPrompt(transcript), raw: true, stream: true, keep_alive: -1,
        options: {
          temperature: 0.2, top_k: 80, repeat_penalty: 1.05, num_predict: maxOutputTokens,
          stop: ["<|im_end|>", "<|im_start|>"],
        },
      } : {
        model,
        messages: [{ role: "system", content: ASSISTANT_SYSTEM_PROMPT }, { role: "user", content: transcript }],
        max_tokens: maxOutputTokens, temperature: 0.45, reasoning_effort: "none", stream: false,
      };
      const response = await fetchImpl(endpoint, {
        method: "POST", headers, signal: controller.signal,
        body: JSON.stringify(requestBody),
      });
      if (!response.ok) throw assistantError("assistant_http_error", String(response.status));
      let firstTokenAt = null;
      let rawAnswer;
      if (provider === "ollama_raw") {
        rawAnswer = await readOllamaStream(response, () => { firstTokenAt ??= performance.now(); });
      } else {
        let payload;
        try { payload = await response.json(); }
        catch { throw assistantError("assistant_invalid_response"); }
        rawAnswer = payload?.choices?.[0]?.message?.content;
        if (String(rawAnswer ?? "").length > 0) firstTokenAt = performance.now();
      }
      const answer = boundedText(rawAnswer, maxReplyChars);
      if (!answer) return { kind: "empty" };
      qwenState = "online";
      const completedAt = performance.now();
      const timings = {
        llm_first_token_ms: firstTokenAt == null ? null : Math.round(firstTokenAt - startedAt),
        llm_request_ms: Math.round(completedAt - startedAt),
      };
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
