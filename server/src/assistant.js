export const ASSISTANT_SYSTEM_PROMPT = [
  "You are Newo, pronounced Neo, a friendly voice assistant.",
  "Answer the user's latest message directly in natural spoken English.",
  "For general knowledge, give two or three useful factual sentences; for simple questions, one sentence is enough.",
  "In the user's message, I, me, and my mean the user, while you and your mean Neo. In your reply, I, me, and my mean Neo, while you and your mean the user.",
  "If unclear, ask one short clarification. Do not use markdown.",
].join(" ");

export const DEFAULT_ASSISTANT_TIME_ZONE = "Asia/Phnom_Penh";
export const ASSISTANT_HISTORY_MAX_EXCHANGES = 3;
export const ASSISTANT_HISTORY_MAX_CHARS_PER_MESSAGE = 240;

export function assistantTimeContext(date, timeZone = DEFAULT_ASSISTANT_TIME_ZONE) {
  const now = date instanceof Date ? date : new Date(date);
  if (Number.isNaN(now.getTime())) throw new RangeError("assistant clock returned an invalid date");
  let dateTime;
  let offset;
  try {
    dateTime = new Intl.DateTimeFormat("en-US", {
      timeZone, weekday: "long", year: "numeric", month: "long", day: "numeric",
      hour: "numeric", minute: "2-digit", second: "2-digit", hour12: true,
    }).format(now);
    const offsetPart = new Intl.DateTimeFormat("en-US", {
      timeZone, timeZoneName: "longOffset",
    }).formatToParts(now).find((part) => part.type === "timeZoneName")?.value;
    if (!offsetPart?.startsWith("GMT")) throw new RangeError("UTC offset is unavailable");
    offset = offsetPart.replace(/^GMT/, "UTC");
  } catch (error) {
    throw new RangeError(`invalid assistant IANA time zone: ${timeZone}`, { cause: error });
  }
  return [
    `Current local date and time: ${dateTime}.`,
    `Timezone: ${timeZone} (${offset}).`,
    "Use this only to answer the user's current date or time question.",
  ].join(" ");
}

class ThinkFilter {
  constructor() { this.pending = ""; this.thinking = false; }

  push(value, final = false) {
    this.pending += String(value ?? "");
    let visible = "";
    while (this.pending) {
      const lower = this.pending.toLowerCase();
      if (this.thinking) {
        const closeAt = lower.indexOf("</think>");
        if (closeAt >= 0) {
          this.pending = this.pending.slice(closeAt + 8);
          this.thinking = false;
          continue;
        }
        const keep = final ? 0 : partialTagSuffix(lower, ["</think>"]);
        this.pending = keep ? this.pending.slice(-keep) : "";
        break;
      }
      const openAt = lower.indexOf("<think>");
      const closeAt = lower.indexOf("</think>");
      const tagAt = openAt < 0 ? closeAt : closeAt < 0 ? openAt : Math.min(openAt, closeAt);
      if (tagAt >= 0) {
        visible += this.pending.slice(0, tagAt);
        const opening = tagAt === openAt;
        this.pending = this.pending.slice(tagAt + (opening ? 7 : 8));
        this.thinking = opening;
        continue;
      }
      const keep = final ? 0 : partialTagSuffix(lower, ["<think>", "</think>"]);
      visible += keep ? this.pending.slice(0, -keep) : this.pending;
      this.pending = keep ? this.pending.slice(-keep) : "";
      break;
    }
    return visible;
  }
}

function partialTagSuffix(value, tags) {
  for (let length = Math.min(value.length, 7); length > 0; --length) {
    const suffix = value.slice(-length);
    if (tags.some((tag) => tag.startsWith(suffix))) return length;
  }
  return 0;
}

function withoutThinking(value) {
  const filter = new ThinkFilter();
  return filter.push(value) + filter.push("", true);
}

function boundedText(value, maxChars) {
  const text = withoutThinking(value).replace(/\s+/g, " ").trim();
  if (!text) return "";
  if (text.length <= maxChars) return text;
  const clipped = text.slice(0, maxChars + 1);
  const boundary = clipped.lastIndexOf(" ");
  const end = boundary >= Math.floor(maxChars * 0.7) ? boundary : maxChars;
  return `${clipped.slice(0, end).replace(/[\s,;:]+$/, "")}…`;
}

function boundedHistoryText(value) {
  const text = withoutThinking(value).replace(/\s+/g, " ").trim();
  if (text.length <= ASSISTANT_HISTORY_MAX_CHARS_PER_MESSAGE) return text;
  const contentLimit = ASSISTANT_HISTORY_MAX_CHARS_PER_MESSAGE - 1;
  const clipped = text.slice(0, contentLimit + 1);
  const boundary = clipped.lastIndexOf(" ");
  const end = boundary >= Math.floor(contentLimit * 0.7) ? boundary : contentLimit;
  return `${clipped.slice(0, end).replace(/[\s,;:]+$/, "")}…`;
}

function normalizedIntent(value) {
  return String(value ?? "")
    .toLowerCase()
    .replace(/[?!.,:;]+$/g, "")
    .replace(/\s+/g, " ")
    .trim();
}

function shouldUseTimeContext(value) {
  const text = normalizedIntent(value);
  return [
    /\bwhat time\b/,
    /\btime is it\b/,
    /\bcurrent time\b/,
    /\bwhat(?:'s| is) the (?:date|day)\b/,
    /\bwhat (?:date|day) is it\b/,
    /\bcurrent date\b/,
    /\btoday(?:'s| is the)? date\b/,
    /\bwhat year is it\b/,
    /\bwhat month is it\b/,
  ].some((pattern) => pattern.test(text));
}

function shouldUseRuntimeContext(value) {
  const text = normalizedIntent(value);
  return /\b(speaker|volume|mute|muted|cloud|connected|connection|device status|your status)\b/.test(text);
}

function shouldUseHistory(value) {
  const text = normalizedIntent(value);
  return [
    /^tell me more$/,
    /^go on$/,
    /^continue$/,
    /^why$/,
    /^how so$/,
    /^what do you mean$/,
    /^what about (that|it|this|him|her|them)$/,
    /^(can you )?explain that$/,
    /^(make|shorten|expand|summarize|explain) (that|it|your last answer|the last answer)\b/,
    /^(and|also|then)\b/,
  ].some((pattern) => pattern.test(text));
}

function memoryShortcut(value, exchanges) {
  const text = normalizedIntent(value);
  const last = exchanges.at(-1);

  if (/^(what did i (just )?(ask|say)( you)?|what was my (last|previous) (question|message))$/.test(text)) {
    return last
      ? { route: "memory_user", text: `You just asked: ${last.user}` }
      : { route: "memory_user", text: "I don't have an earlier question in this session." };
  }

  if (/^(what did you (just )?(say|tell)( me)?|what was your (last|previous) (answer|reply|response))$/.test(text)) {
    return last
      ? { route: "memory_assistant", text: `I just said: ${last.assistant}` }
      : { route: "memory_assistant", text: "I haven't given an earlier answer in this session." };
  }

  if (/^(repeat (that|your last answer)|say (that|it) again)$/.test(text)) {
    return last
      ? { route: "memory_repeat", text: last.assistant }
      : { route: "memory_repeat", text: "I don't have an earlier answer to repeat." };
  }

  return null;
}

function assistantError(code, detail) {
  const error = new Error(detail ? `${code}: ${detail}` : code);
  error.code = code;
  return error;
}

function rawLfmPrompt(messages) {
  const [system, ...conversation] = messages;
  const systemText = `${system.content}\n\nNO-THINK MODE IS ACTIVE.\nDo not produce chain-of-thought, hidden analysis, plans, drafts, or self-talk.\nIf thinking begins, close it immediately.\nPut only the useful response after </think>.`;
  const turns = conversation.map((message) => `<|im_start|>${message.role}\n${message.content}\n<|im_end|>`).join("\n");
  return `<|startoftext|><|im_start|>system\n${systemText}\n<|im_end|>\n${turns}\n<|im_start|>assistant\n<think>\nNo unnecessary reasoning. Close thinking and answer immediately.\n</think>\n`;
}

async function readOllamaStream(response, { onFirstRawToken, onFirstSpeakableToken }) {
  if (!response.body) throw assistantError("assistant_invalid_response");
  const decoder = new TextDecoder();
  const thinkFilter = new ThinkFilter();
  let pending = "";
  let answer = "";
  const consume = (line) => {
    if (!line.trim()) return;
    let payload;
    try { payload = JSON.parse(line); }
    catch { throw assistantError("assistant_invalid_response"); }
    if (payload.error) throw assistantError("assistant_request_failed", String(payload.error));
    if (typeof payload.response === "string" && payload.response.length > 0) {
      onFirstRawToken();
      const visible = thinkFilter.push(payload.response);
      if (/\S/.test(visible)) onFirstSpeakableToken();
      answer += visible;
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
  const tail = thinkFilter.push("", true);
  if (/\S/.test(tail)) onFirstSpeakableToken();
  return answer + tail;
}

function createOllamaTransport() {
  const agents = {
    "http:": new http.Agent({ keepAlive: true }),
    "https:": new https.Agent({ keepAlive: true }),
  };
  function request(url, { method = "GET", headers, body, signal } = {}) {
    return new Promise((resolve, reject) => {
      const target = new URL(url);
      const client = target.protocol === "https:" ? https : http;
      const req = client.request(target, { method, headers, agent: agents[target.protocol], signal }, (incoming) => {
        resolve({
          ok: incoming.statusCode >= 200 && incoming.statusCode < 300,
          status: incoming.statusCode,
          body: incoming,
          async json() {
            let text = "";
            for await (const chunk of incoming) text += chunk.toString("utf8");
            return JSON.parse(text);
          },
        });
      });
      req.on("error", reject);
      req.end(body);
    });
  }
  function close() { for (const agent of Object.values(agents)) agent.destroy(); }
  return { request, close };
}

function runtimeContextMessage(context) {
  if (!context || typeof context !== "object") return null;
  const parts = [];
  if (typeof context.speakerEnabled === "boolean") parts.push(`speaker=${context.speakerEnabled ? "enabled" : "disabled"}`);
  if (Number.isInteger(context.speakerVolume) && context.speakerVolume >= 0 && context.speakerVolume <= 100)
    parts.push(`volume=${context.speakerVolume}%`);
  if (typeof context.speakerMuted === "boolean") parts.push(`mute=${context.speakerMuted ? "on" : "off"}`);
  if (context.cloudStatus) parts.push(`cloud=${boundedText(context.cloudStatus, 24)}`);
  return parts.length ? `Current runtime state: ${parts.join(", ")}.` : null;
}

/** A bounded provider-selectable client for one finalized voice turn. */
export function createAssistantRuntime({
  enabled = false, provider = "openai_chat", baseUrl, model, apiKey, timeoutMs = 15_000, maxOutputTokens = 72,
  maxReplyChars = 300, timeZone = DEFAULT_ASSISTANT_TIME_ZONE, now = () => new Date(),
  runtimeContext = null, fetchImpl, logger = null,
} = {}) {
  assistantTimeContext(new Date(0), timeZone);
  const active = new Map();
  let closing = false;
  let providerState = enabled ? "unknown" : "disabled";
  const ollamaTransport = !fetchImpl && provider === "ollama_raw" ? createOllamaTransport() : null;
  const requestImpl = fetchImpl ?? ollamaTransport?.request ?? fetch;
  const history = new Map();
  const base = baseUrl ? String(baseUrl).replace(/\/+$/, "") : null;
  const endpoint = base ? `${base}${provider === "ollama_raw" ? "/api/generate" : "/v1/chat/completions"}` : null;
  const modelsEndpoint = base ? `${base}${provider === "ollama_raw" ? "/api/tags" : "/v1/models"}` : null;

  function getTelemetry() {
    return { enabled, provider, model: model ?? null, online: providerState, qwen: providerState, active: active.size > 0 };
  }

  function storeExchange(deviceId, user, assistant) {
    const previous = history.get(deviceId) ?? [];
    const next = [...previous, {
      user: boundedHistoryText(user),
      assistant: boundedHistoryText(assistant),
    }].slice(-ASSISTANT_HISTORY_MAX_EXCHANGES);
    history.set(deviceId, next);
  }

  async function refreshHealth() {
    if (!enabled) return getTelemetry();
    if (!modelsEndpoint || !model) {
      providerState = "offline";
      return getTelemetry();
    }

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), Math.min(timeoutMs, 1_000));
    timer.unref();

    try {
      const headers = apiKey ? { authorization: `Bearer ${apiKey}` } : undefined;
      const response = await requestImpl(modelsEndpoint, { headers, signal: controller.signal });
      const payload = response.ok ? await response.json() : null;
      const models = provider === "ollama_raw" ? payload?.models : payload?.data;
      providerState = Array.isArray(models) && models.some((item) => {
        const candidate = provider === "ollama_raw" ? item?.name ?? item?.model : item?.id;
        return candidate === model || (provider === "ollama_raw" && !model.includes(":") && candidate === `${model}:latest`);
      }) ? "online" : "offline";
    } catch {
      providerState = "offline";
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

    const previousExchanges = history.get(deviceId) ?? [];
    const historyAvailable = previousExchanges.length;
    const shortcut = memoryShortcut(transcript, previousExchanges);

    if (shortcut) {
      const answer = boundedText(shortcut.text, maxReplyChars);
      const timings = {
        llm_request_ms: 0,
        history_turns: 0,
        history_available: historyAvailable,
        history_used: 0,
        prompt_chars: 0,
        route: shortcut.route,
      };

      storeExchange(deviceId, transcript, answer);

      logger?.info({
        device_id: deviceId,
        stream_id: streamId,
        query_text: transcript,
        reply_chars: answer.length,
        reply_text: answer,
        ...timings,
      }, "Assistant text ready");

      return { kind: "response", text: answer, timings };
    }

    const controller = new AbortController();
    const startedAt = performance.now();
    const timer = setTimeout(
      () => controller.abort(assistantError("assistant_timeout")),
      timeoutMs,
    );
    timer.unref();
    active.set(deviceId, controller);

    try {
      const usedExchanges = shouldUseHistory(transcript)
        ? previousExchanges.slice(-1)
        : [];

      const includeTime = shouldUseTimeContext(transcript);
      const includeRuntime = shouldUseRuntimeContext(transcript);
      const stateMessage = includeRuntime
        ? runtimeContextMessage(runtimeContext?.({ deviceId, streamId }))
        : null;

      const systemParts = [ASSISTANT_SYSTEM_PROMPT];
      if (includeTime) systemParts.push(assistantTimeContext(now(), timeZone));
      if (stateMessage) systemParts.push(stateMessage);

      const messages = [
        { role: "system", content: systemParts.join(" ") },
        ...usedExchanges.flatMap((exchange) => [
          { role: "user", content: exchange.user },
          { role: "assistant", content: exchange.assistant },
        ]),
        { role: "user", content: transcript },
      ];

      const historyUsed = usedExchanges.length;
      const promptChars = messages.reduce(
        (total, message) => total + message.content.length,
        0,
      );

      logger?.info({
        device_id: deviceId,
        stream_id: streamId,
        transcript_chars: transcript.length,
        query_text: transcript,
        history_available: historyAvailable,
        history_used: historyUsed,
        history_turns: historyUsed,
        time_context: includeTime,
        runtime_context: Boolean(stateMessage),
        prompt_chars: promptChars,
      }, "Assistant LLM request started");

      const headers = { "content-type": "application/json" };
      if (apiKey) headers.authorization = `Bearer ${apiKey}`;

      const requestBody = provider === "ollama_raw" ? {
        model,
        prompt: rawLfmPrompt(messages),
        raw: true,
        stream: true,
        keep_alive: -1,
        options: {
          num_predict: maxOutputTokens,
          temperature: 0.2,
          top_k: 80,
          repeat_penalty: 1.05,
          stop: ["<|im_end|>", "<|im_start|>"],
        },
      } : {
        model,
        messages,
        max_tokens: maxOutputTokens,
        temperature: 0.7,
        top_p: 0.8,
        top_k: 20,
        min_p: 0,
        stream: false,
      };

      const response = await requestImpl(endpoint, {
        method: "POST",
        headers,
        signal: controller.signal,
        body: JSON.stringify(requestBody),
      });

      if (!response.ok) {
        throw assistantError("assistant_http_error", String(response.status));
      }

      let payload = null;
      let firstRawTokenAt = null;
      let firstTokenAt = null;
      let rawAnswer;
      if (provider === "ollama_raw") {
        rawAnswer = await readOllamaStream(response, {
          onFirstRawToken: () => { firstRawTokenAt ??= performance.now(); },
          onFirstSpeakableToken: () => { firstTokenAt ??= performance.now(); },
        });
      } else {
        try {
          payload = await response.json();
        } catch {
          throw assistantError("assistant_invalid_response");
        }
        rawAnswer = payload?.choices?.[0]?.message?.content;
        if (String(rawAnswer ?? "").length > 0) firstRawTokenAt = firstTokenAt = performance.now();
      }

      const answer = boundedText(rawAnswer, maxReplyChars);

      if (!answer) return { kind: "empty" };

      storeExchange(deviceId, transcript, answer);
      providerState = "online";

      const completedAt = performance.now();
      const timings = {
        llm_first_raw_token_ms: firstRawTokenAt == null ? null : Math.round(firstRawTokenAt - startedAt),
        llm_first_token_ms: firstTokenAt == null ? null : Math.round(firstTokenAt - startedAt),
        llm_request_ms: Math.round(completedAt - startedAt),
        history_turns: historyUsed,
        history_available: historyAvailable,
        history_used: historyUsed,
        prompt_chars: promptChars,
        route: "llm",
      };

      if (Number.isFinite(payload?.usage?.prompt_tokens)) {
        timings.input_tokens = payload.usage.prompt_tokens;
      }

      if (Number.isFinite(payload?.usage?.completion_tokens)) {
        timings.output_tokens = payload.usage.completion_tokens;
      }

      logger?.info({
        device_id: deviceId,
        stream_id: streamId,
        query_text: transcript,
        reply_chars: answer.length,
        reply_text: answer,
        time_context: includeTime,
        runtime_context: Boolean(stateMessage),
        ...timings,
      }, "Assistant text ready");

      return {
        kind: "response",
        text: answer,
        timings,
      };
    } catch (error) {
      providerState = "offline";

      const code = controller.signal.aborted
        ? controller.signal.reason?.code ?? "assistant_cancelled"
        : error?.code ?? "assistant_request_failed";

      logger?.warn({
        device_id: deviceId,
        stream_id: streamId,
        error_code: code,
      }, "Assistant LLM request failed");

      return {
        kind: code === "assistant_timeout" ? "timeout" : "error",
        error: code,
      };
    } finally {
      clearTimeout(timer);

      if (active.get(deviceId) === controller) {
        active.delete(deviceId);
      }
    }
  }

  function abortDevice(deviceId) {
    active.get(deviceId)?.abort(assistantError("assistant_cancelled"));
    history.delete(deviceId);
  }

  function close() {
    closing = true;

    for (const controller of active.values()) {
      controller.abort(assistantError("assistant_shutdown"));
    }

    history.clear();
    ollamaTransport?.close();
  }

  return {
    respond,
    refreshHealth,
    getTelemetry,
    abortDevice,
    close,
    isActive: (deviceId) => active.has(deviceId),
  };
}
import http from "node:http";
import https from "node:https";
