import http from "node:http";
import https from "node:https";

import {
  createAssistantProfiles,
  LFM_PROFILE_ID,
  QWEN_PROFILE_ID,
  QWEN_SYSTEM_PROMPT,
  resolveAssistantProfile,
} from "./assistant-profiles.js";

export const ASSISTANT_SYSTEM_PROMPT = QWEN_SYSTEM_PROMPT;

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
  runtimeContext = null, fetchImpl, logger = null, profiles = null, preferredProfile = null,
  fallbackCooldownMs = 30_000,
} = {}) {
  assistantTimeContext(new Date(0), timeZone);
  const active = new Map();
  let closing = false;
  const history = new Map();
  const profileMode = profiles != null || preferredProfile != null;
  let configuredProfiles = profiles ?? createAssistantProfiles({ qwenApiKey: apiKey });
  const requestedId = resolveAssistantProfile(preferredProfile, configuredProfiles) ??
    (provider === "ollama_raw" ? LFM_PROFILE_ID : QWEN_PROFILE_ID);

  // Keep the original constructor contract for focused provider tests and older callers.
  if (!profileMode) {
    const original = configuredProfiles[requestedId];
    configuredProfiles = {
      ...configuredProfiles,
      [requestedId]: {
        ...original,
        baseUrl: baseUrl ? String(baseUrl).replace(/\/+$/, "") : original.baseUrl,
        model: model ?? original.model,
        apiKey: apiKey ?? original.apiKey,
        timeoutMs,
        maxOutputTokens,
        maxReplyChars,
        fallbackProfile: null,
      },
    };
  }

  let preferredId = requestedId;
  let effectiveId = requestedId;
  let fallbackReason = null;
  let lastFallbackAt = 0;
  const health = new Map(Object.keys(configuredProfiles).map((id) => [id, enabled ? "unknown" : "disabled"]));
  const ollamaTransport = !fetchImpl ? createOllamaTransport() : null;
  const requestImpl = fetchImpl ?? ollamaTransport?.request ?? fetch;

  const profileById = (id) => configuredProfiles[id] ?? null;
  const endpointFor = (profile, healthCheck = false) => {
    const suffix = healthCheck ? profile.health.endpoint : profile.endpoint;
    return profile.baseUrl ? `${String(profile.baseUrl).replace(/\/+$/, "")}${suffix}` : null;
  };

  function getTelemetry() {
    const profile = profileById(effectiveId);
    const profileHealth = Object.fromEntries(health);
    const online = enabled ? health.get(effectiveId) ?? "unknown" : "disabled";
    return {
      enabled,
      preferred_profile: preferredId,
      effective_profile: effectiveId,
      fallback_active: preferredId !== effectiveId,
      fallback_reason: fallbackReason,
      provider: profile?.provider ?? null,
      model: profile?.model ?? null,
      online,
      qwen: online,
      profile_health: profileHealth,
      active: active.size > 0,
    };
  }

  function storeExchange(deviceId, user, assistant) {
    const previous = history.get(deviceId) ?? [];
    const next = [...previous, {
      user: boundedHistoryText(user),
      assistant: boundedHistoryText(assistant),
    }].slice(-ASSISTANT_HISTORY_MAX_EXCHANGES);
    history.set(deviceId, next);
  }

  async function refreshProfileHealth(id) {
    const profile = profileById(id);
    if (!enabled || !profile?.enabled || !endpointFor(profile, true) || !profile.model) {
      health.set(id, enabled ? "offline" : "disabled");
      return health.get(id);
    }
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), Math.min(profile.timeoutMs, 1_000));
    timer.unref();
    try {
      const headers = profile.apiKey ? { authorization: `Bearer ${profile.apiKey}` } : undefined;
      const response = await requestImpl(endpointFor(profile, true), { headers, signal: controller.signal });
      const payload = response.ok ? await response.json() : null;
      const models = profile.provider === "ollama_raw" ? payload?.models : payload?.data;
      const online = Array.isArray(models) && models.some((item) => {
        const candidate = profile.provider === "ollama_raw" ? item?.name ?? item?.model : item?.id;
        return candidate === profile.model ||
          (profile.health.implicitLatestTag && !profile.model.includes(":") && candidate === `${profile.model}:latest`);
      });
      health.set(id, online ? "online" : "offline");
    } catch {
      health.set(id, "offline");
    } finally {
      clearTimeout(timer);
    }
    return health.get(id);
  }

  async function refreshHealth() {
    if (!enabled) return getTelemetry();
    await refreshProfileHealth(preferredId);
    if (effectiveId !== preferredId) await refreshProfileHealth(effectiveId);
    if (effectiveId !== preferredId && health.get(preferredId) === "online") recoverPreferred("health_check");
    return getTelemetry();
  }

  function recoverPreferred(reason) {
    const from = effectiveId;
    effectiveId = preferredId;
    fallbackReason = null;
    logger?.info({ preferred_profile: preferredId, effective_profile: effectiveId, previous_profile: from, recovery_reason: reason }, "Assistant profile recovered");
  }

  async function setPreferredProfile(value) {
    const id = resolveAssistantProfile(value, configuredProfiles);
    if (!id || !profileById(id)?.enabled) throw assistantError("assistant_profile_invalid", String(value ?? ""));
    const previous = preferredId;
    preferredId = id;
    effectiveId = id;
    fallbackReason = null;
    await refreshProfileHealth(id);
    logger?.info({ previous_profile: previous, preferred_profile: id, effective_profile: id, profile_health: health.get(id) }, "Assistant profile switched");
    return getTelemetry();
  }

  async function requestProfile(profile, { deviceId, streamId, transcript, previousExchanges, historyAvailable }) {
    const endpoint = endpointFor(profile);
    if (!profile?.enabled || !endpoint || !profile.model) return { kind: "unavailable", error: "assistant_unavailable", availabilityFailure: true };
    const controller = new AbortController();
    const startedAt = performance.now();
    const timer = setTimeout(() => controller.abort(assistantError("assistant_timeout")), profile.timeoutMs);
    timer.unref();
    active.set(deviceId, controller);

    try {
      const usedExchanges = profile.contextPolicy.selectiveHistory && shouldUseHistory(transcript)
        ? previousExchanges.slice(-1) : [];
      const includeTime = profile.contextPolicy.selectiveTimeContext && shouldUseTimeContext(transcript);
      const includeRuntime = profile.contextPolicy.selectiveRuntimeContext && shouldUseRuntimeContext(transcript);
      const stateMessage = includeRuntime ? runtimeContextMessage(runtimeContext?.({ deviceId, streamId })) : null;
      const systemParts = [profile.systemPrompt];
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
      const promptChars = messages.reduce((total, message) => total + message.content.length, 0);
      logger?.info({ device_id: deviceId, stream_id: streamId, preferred_profile: preferredId,
        effective_profile: profile.id, fallback_active: profile.id !== preferredId, provider: profile.provider,
        model: profile.model, transcript_chars: transcript.length, query_text: transcript,
        history_available: historyAvailable, history_used: historyUsed, history_turns: historyUsed,
        time_context: includeTime, runtime_context: Boolean(stateMessage), prompt_chars: promptChars },
      "Assistant LLM request started");

      const headers = { "content-type": "application/json" };
      if (profile.apiKey) headers.authorization = `Bearer ${profile.apiKey}`;
      const requestBody = profile.provider === "ollama_raw" ? {
        model: profile.model,
        prompt: rawLfmPrompt(messages),
        raw: profile.requestOptions.raw,
        stream: profile.requestOptions.stream,
        keep_alive: profile.keepAlive,
        options: {
          num_predict: profile.maxOutputTokens,
          ...profile.sampling,
          stop: ["<|im_end|>", "<|im_start|>"],
        },
      } : {
        model: profile.model,
        messages,
        max_tokens: profile.maxOutputTokens,
        ...profile.sampling,
        stream: profile.requestOptions.stream,
      };
      const response = await requestImpl(endpoint, { method: "POST", headers, signal: controller.signal, body: JSON.stringify(requestBody) });
      if (!response.ok) throw assistantError("assistant_http_error", String(response.status));

      let payload = null;
      let firstRawTokenAt = null;
      let firstTokenAt = null;
      let rawAnswer;
      if (profile.provider === "ollama_raw") {
        rawAnswer = await readOllamaStream(response, {
          onFirstRawToken: () => { firstRawTokenAt ??= performance.now(); },
          onFirstSpeakableToken: () => { firstTokenAt ??= performance.now(); },
        });
      } else {
        try { payload = await response.json(); }
        catch { throw assistantError("assistant_invalid_response"); }
        rawAnswer = payload?.choices?.[0]?.message?.content;
        if (String(rawAnswer ?? "").length > 0) firstRawTokenAt = firstTokenAt = performance.now();
      }
      const answer = boundedText(rawAnswer, profile.maxReplyChars);
      if (!answer) return { kind: "empty", availabilityFailure: false };
      health.set(profile.id, "online");
      const completedAt = performance.now();
      const timings = {
        llm_first_raw_token_ms: firstRawTokenAt == null ? null : Math.round(firstRawTokenAt - startedAt),
        llm_first_token_ms: firstTokenAt == null ? null : Math.round(firstTokenAt - startedAt),
        llm_request_ms: Math.round(completedAt - startedAt),
        history_turns: historyUsed, history_available: historyAvailable, history_used: historyUsed,
        prompt_chars: promptChars, route: "llm", preferred_profile: preferredId,
        effective_profile: profile.id, fallback_active: profile.id !== preferredId,
        fallback_reason: profile.id !== preferredId ? fallbackReason : null,
        provider: profile.provider, model: profile.model,
      };
      if (Number.isFinite(payload?.usage?.prompt_tokens)) timings.input_tokens = payload.usage.prompt_tokens;
      if (Number.isFinite(payload?.usage?.completion_tokens)) timings.output_tokens = payload.usage.completion_tokens;
      logger?.info({ device_id: deviceId, stream_id: streamId, query_text: transcript,
        reply_chars: answer.length, reply_text: answer, time_context: includeTime,
        runtime_context: Boolean(stateMessage), ...timings }, "Assistant text ready");
      return { kind: "response", text: answer, timings, profileId: profile.id, availabilityFailure: false };
    } catch (error) {
      const code = controller.signal.aborted ? controller.signal.reason?.code ?? "assistant_cancelled" : error?.code ?? "assistant_request_failed";
      const availabilityFailure = ["assistant_timeout", "assistant_http_error", "assistant_request_failed"].includes(code);
      if (availabilityFailure) health.set(profile.id, "offline");
      logger?.warn({ device_id: deviceId, stream_id: streamId, preferred_profile: preferredId,
        effective_profile: profile.id, provider: profile.provider, model: profile.model, error_code: code },
      "Assistant LLM request failed");
      return { kind: code === "assistant_timeout" ? "timeout" : "error", error: code, availabilityFailure };
    } finally {
      clearTimeout(timer);
      if (active.get(deviceId) === controller) active.delete(deviceId);
    }
  }

  async function respond({ deviceId, streamId, text }) {
    const transcript = boundedText(text, 800);

    if (!enabled || closing) return { kind: "disabled" };
    if (!transcript) return { kind: "empty" };
    if (active.has(deviceId)) return { kind: "busy" };

    const previousExchanges = history.get(deviceId) ?? [];
    const historyAvailable = previousExchanges.length;
    const shortcut = memoryShortcut(transcript, previousExchanges);

    if (shortcut) {
      const selected = profileById(effectiveId);
      const answer = boundedText(shortcut.text, selected?.maxReplyChars ?? maxReplyChars);
      const timings = {
        llm_request_ms: 0,
        history_turns: 0,
        history_available: historyAvailable,
        history_used: 0,
        prompt_chars: 0,
        route: shortcut.route,
        preferred_profile: preferredId,
        effective_profile: effectiveId,
        fallback_active: preferredId !== effectiveId,
        fallback_reason: fallbackReason,
        provider: selected?.provider ?? null,
        model: selected?.model ?? null,
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

    if (effectiveId !== preferredId && Date.now() - lastFallbackAt >= fallbackCooldownMs) {
      if (await refreshProfileHealth(preferredId) === "online") recoverPreferred("cooldown_health_check");
      else lastFallbackAt = Date.now();
    }
    let selected = profileById(effectiveId);
    let result = await requestProfile(selected, { deviceId, streamId, transcript, previousExchanges, historyAvailable });
    const fallbackId = selected?.fallbackProfile;
    if (result.availabilityFailure && fallbackId && fallbackId !== selected.id && profileById(fallbackId)?.enabled) {
      fallbackReason = result.error;
      effectiveId = fallbackId;
      lastFallbackAt = Date.now();
      logger?.warn({ preferred_profile: preferredId, failed_profile: selected.id, effective_profile: fallbackId,
        fallback_reason: fallbackReason }, "Assistant profile fallback activated");
      selected = profileById(fallbackId);
      result = await requestProfile(selected, { deviceId, streamId, transcript, previousExchanges, historyAvailable });
    }
    if (result.kind === "response") storeExchange(deviceId, transcript, result.text);
    const { availabilityFailure: _availabilityFailure, profileId: _profileId, ...publicResult } = result;
    return publicResult;
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
    refreshProfileHealth,
    setPreferredProfile,
    getTelemetry,
    abortDevice,
    close,
    isActive: (deviceId) => active.has(deviceId),
  };
}
