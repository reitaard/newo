function serviceError(code, detail = "") {
  const error = new Error(detail || code);
  error.code = code;
  return error;
}

async function streamOllamaChat(url, body, { fetchImpl = fetch, timeoutMs = 15_000, signal } = {}) {
  const controller = new AbortController();
  const relay = () => controller.abort(signal?.reason ?? serviceError("xiaomei_cancelled"));
  if (signal?.aborted) relay(); else signal?.addEventListener("abort", relay, { once: true });
  const timer = setTimeout(() => controller.abort(serviceError("xiaomei_timeout")), timeoutMs);
  timer.unref?.();
  const started = performance.now();
  let firstContentAt = null;
  let text = "";
  let thinking = "";
  let finalChunk = null;
  try {
    const response = await fetchImpl(url, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ ...body, stream: true }),
      signal: controller.signal,
    });
    if (!response.ok || !response.body) {
      throw serviceError("xiaomei_service_error", `${response.status} ${await response.text()}`);
    }
    const decoder = new TextDecoder();
    let buffer = "";
    for await (const chunk of response.body) {
      buffer += decoder.decode(chunk, { stream: true });
      while (true) {
        const newline = buffer.indexOf("\n");
        if (newline < 0) break;
        const line = buffer.slice(0, newline).trim();
        buffer = buffer.slice(newline + 1);
        if (!line) continue;
        const item = JSON.parse(line);
        if (item.message?.thinking) thinking += item.message.thinking;
        if (item.message?.content) {
          firstContentAt ??= performance.now();
          text += item.message.content;
        }
        if (item.done) finalChunk = item;
      }
    }
    buffer += decoder.decode();
    if (buffer.trim()) {
      const item = JSON.parse(buffer.trim());
      if (item.message?.thinking) thinking += item.message.thinking;
      if (item.message?.content) {
        firstContentAt ??= performance.now();
        text += item.message.content;
      }
      if (item.done) finalChunk = item;
    }
  } catch (error) {
    if (controller.signal.aborted) throw controller.signal.reason ?? serviceError("xiaomei_cancelled");
    throw error;
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener("abort", relay);
  }
  const completedAt = performance.now();
  const content = text.trim();
  if (!content) throw serviceError("xiaomei_empty_response");
  return {
    text: content,
    thinking: thinking.trim(),
    latencyMs: Math.round(completedAt - started),
    firstContentMs: firstContentAt == null ? null : Math.round(firstContentAt - started),
    loadMs: Math.round(Number(finalChunk?.load_duration ?? 0) / 1e6),
    promptEvalCount: finalChunk?.prompt_eval_count ?? null,
    evalCount: finalChunk?.eval_count ?? null,
    doneReason: finalChunk?.done_reason ?? null,
  };
}

export class XiaomeiOllamaClient {
  constructor({ baseUrl, model, timeoutMs = 15_000, fetchImpl = fetch, logger = null, role = "model", keepAlive = -1 }) {
    this.baseUrl = String(baseUrl).replace(/\/+$/, "");
    this.model = model;
    this.timeoutMs = timeoutMs;
    this.fetchImpl = fetchImpl;
    this.logger = logger;
    this.role = role;
    this.keepAlive = keepAlive;
  }

  async complete({ system, user, messages = null, temperature = 0.2, topP = 0.9, topK = 20,
    maxTokens = 240, contextSize = 4096, format = null, signal, keepAlive = this.keepAlive }) {
    const inputMessages = Array.isArray(messages) ? messages : [
      ...(system ? [{ role: "system", content: system }] : []),
      { role: "user", content: String(user ?? "") },
    ];
    const body = {
      model: this.model,
      messages: inputMessages,
      think: false,
      keep_alive: keepAlive,
      options: {
        num_ctx: contextSize,
        temperature,
        top_p: topP,
        top_k: topK,
        num_predict: maxTokens,
      },
      ...(format ? { format } : {}),
    };
    const result = await streamOllamaChat(`${this.baseUrl}/api/chat`, body,
      { fetchImpl: this.fetchImpl, timeoutMs: this.timeoutMs, signal });
    this.logger?.info?.({ event: "XIAOMEI_MODEL", role: this.role, model: this.model,
      latency_ms: result.latencyMs, first_content_ms: result.firstContentMs, load_ms: result.loadMs },
    "Xiaomei model completed");
    return { ...result, model: this.model };
  }

  async setResident(resident) {
    const response = await this.fetchImpl(`${this.baseUrl}/api/generate`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ model: this.model, prompt: "", stream: false, keep_alive: resident ? -1 : 0 }),
    });
    if (!response.ok) throw serviceError("xiaomei_residency_failed", `${response.status} ${await response.text()}`);
    return resident;
  }
}

export class HyMtTranslationClient extends XiaomeiOllamaClient {
  async translate({ text, sourceLanguage = "auto", targetLanguage = "auto", preserveTone = true,
    registerMode = "natural", signal }) {
    const system = [
      "Translate the user's utterance only. Output only the translation.",
      "Preserve intended meaning, names, dates, numbers, negation, slang, emotion, and register.",
      preserveTone ? "Preserve tone and intensity; do not deliberately soften profanity." : "Prefer neutral natural register.",
      registerMode === "literal" ? "Prefer a close literal rendering when it remains intelligible." : "Prefer natural conversational phrasing in the target language.",
      `Source language: ${sourceLanguage}. Target language: ${targetLanguage}.`,
    ].join(" ");
    return this.complete({ system, user: text, temperature: 0, maxTokens: 256, signal });
  }
}

export const XIAOMEI_PERSONA_PROMPT = "You are Xiaomei (玲玲), a concise bilingual Mandarin-English voice companion and Chinese teacher. Speak naturally for audio, use no markdown, and never invent exact pinyin or tones. Keep ordinary answers brief.";

export function teachingPrompt(verified = null, context = null) {
  return [
    XIAOMEI_PERSONA_PROMPT,
    "Teach practically: give the useful answer first, then a short explanation and one natural example.",
    verified ? `Authoritative pronunciation data: ${JSON.stringify(verified)}. Treat it as the source of truth.` :
      "If exact pinyin or tone data is required and no authoritative data is supplied, say it needs verification rather than guessing.",
    context ? `Relevant prior teaching context: ${JSON.stringify(context)}.` : "",
  ].filter(Boolean).join(" ");
}

export function interpreterMetaPrompt(session) {
  return [
    XIAOMEI_PERSONA_PROMPT,
    "Explain the recent interpreted speech to the user; do not relay this question as a translation.",
    `Recent interpreter context: ${JSON.stringify(session.interpreter.recent)}.`,
  ].join(" ");
}

export class NullPronunciationProvider {
  async lookup() { return null; }
}
