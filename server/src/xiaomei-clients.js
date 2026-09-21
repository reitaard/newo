function serviceError(code, detail = "") { const error = new Error(detail || code); error.code = code; return error; }

async function jsonRequest(url, body, { fetchImpl = fetch, timeoutMs = 15_000, signal } = {}) {
  const controller = new AbortController();
  const relay = () => controller.abort(signal.reason ?? serviceError("xiaomei_cancelled"));
  if (signal?.aborted) relay(); else signal?.addEventListener("abort", relay, { once: true });
  const timer = setTimeout(() => controller.abort(serviceError("xiaomei_timeout")), timeoutMs); timer.unref?.();
  try {
    const response = await fetchImpl(url, { method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify(body), signal: controller.signal });
    if (!response.ok) throw serviceError("xiaomei_service_error", `${response.status} ${await response.text()}`);
    return await response.json();
  } catch (error) {
    if (controller.signal.aborted) throw controller.signal.reason ?? serviceError("xiaomei_cancelled");
    throw error;
  } finally { clearTimeout(timer); signal?.removeEventListener("abort", relay); }
}

export class XiaomeiModelClient {
  constructor({ baseUrl, model, timeoutMs = 15_000, fetchImpl = fetch, logger = null, role = "model" }) {
    this.baseUrl = String(baseUrl).replace(/\/+$/, ""); this.model = model; this.timeoutMs = timeoutMs;
    this.fetchImpl = fetchImpl; this.logger = logger; this.role = role;
  }
  async complete({ system, user, temperature = 0.2, maxTokens = 240, responseFormat, signal }) {
    const started = performance.now();
    const body = { model: this.model, messages: [{ role: "system", content: system }, { role: "user", content: user }],
      temperature, max_tokens: maxTokens, stream: false, ...(responseFormat ? { response_format: responseFormat } : {}) };
    const payload = await jsonRequest(`${this.baseUrl}/v1/chat/completions`, body,
      { fetchImpl: this.fetchImpl, timeoutMs: this.timeoutMs, signal });
    const text = payload?.choices?.[0]?.message?.content?.trim();
    if (!text) throw serviceError("xiaomei_empty_response");
    const latencyMs = Math.round(performance.now() - started);
    this.logger?.info({ event: "XIAOMEI_MODEL", role: this.role, model: this.model, latency_ms: latencyMs }, "Xiaomei model completed");
    return { text, latencyMs, model: this.model };
  }
}

export class HyMtTranslationClient extends XiaomeiModelClient {
  async translate({ text, sourceLanguage = "auto", targetLanguage = "auto", preserveTone = true, signal }) {
    const system = ["Translate the user's utterance only. Return only the natural conversational translation.",
      "Preserve meaning, names, dates, numbers, negation, slang, emotion, and register.",
      preserveTone ? "Preserve tone and intensity; do not soften profanity." : "Prefer neutral natural register.",
      `Source language: ${sourceLanguage}. Target language: ${targetLanguage}.`].join(" ");
    return this.complete({ system, user: text, temperature: 0, maxTokens: 256, signal });
  }
}

export const XIAOMEI_PERSONA_PROMPT = "You are Xiaomei (玲玲), a warm, concise bilingual Mandarin-English voice companion and Chinese teacher. Speak naturally for audio, use no markdown, and never invent exact pinyin or tones. Keep ordinary answers brief.";

export function teachingPrompt(verified = null) {
  return [XIAOMEI_PERSONA_PROMPT, "Teach practically: answer first, then a short explanation and one natural example.",
    verified ? `Authoritative pronunciation data: ${JSON.stringify(verified)}. Treat it as the source of truth.` :
      "If exact pinyin or tone data is required and no authoritative data is supplied, say it needs verification rather than guessing."].join(" ");
}

export function interpreterMetaPrompt(session) {
  return [XIAOMEI_PERSONA_PROMPT, "Explain the recent interpreted speech to the user; do not relay the question as a translation.",
    `Recent interpreter context: ${JSON.stringify(session.interpreter.recent)}.`].join(" ");
}

export class NullPronunciationProvider {
  async lookup() { return null; }
}
