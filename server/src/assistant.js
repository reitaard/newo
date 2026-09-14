import http from "node:http";
import https from "node:https";

import {
  createAssistantProfiles,
  LFM_PROFILE_ID,
  QWEN_PROFILE_ID,
  QWEN_SYSTEM_PROMPT,
  profileTuning,
  resolveAssistantProfile,
} from "./assistant-profiles.js";
import { routeAssistantRequest } from "./assistant-routing.js";
import { lfmToolDefinitions, parseLfmToolCalls, validateToolCall } from "./assistant-tools.js";
import { toolsForCapability } from "./capability-router.js";
import { structuredCapabilityContext } from "./assistant-capabilities.js";

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

function isDirectCurrentTimeIntent(value) {
  const text = normalizedIntent(value);
  let intent = text.replace(/\s+(?:with|including|and) seconds?$/, "");
  const pattern = /^(?:please\s+)?(?:(?:can|could|would) you (?:please )?)?(?:what time is it|what(?:'s| is) the time|tell me (?:what time (?:it is|is it)|the time)|give me (?:the )?(?:current )?time|current time)(?: right now| now| please)?$/;
  if (pattern.test(intent)) return true;
  const withoutVocative = intent.replace(/\s+[a-z][a-z'-]*$/, "");
  return withoutVocative !== intent && pattern.test(withoutVocative);
}

function shouldUseTimeContext(value) {
  const text = normalizedIntent(value);
  if (isDirectCurrentTimeIntent(text)) return true;
  return [
    /\bwhat time\b/,
    /\btime is it\b/,
    /\bwhat(?:'s| is) the time\b/,
    /\bcurrent time\b/,
    /\b(?:tell|give) me (?:the )?(?:current )?time\b/,
    /\b(?:can|could|would) you (?:please )?(?:tell|give) me (?:the )?(?:current )?time\b/,
    /\bwhat(?:'s| is) the (?:date|day)\b/,
    /\bwhat (?:date|day) is it\b/,
    /\bcurrent date\b/,
    /\btoday(?:'s| is the)? date\b/,
    /\bwhat year is it\b/,
    /\bwhat month is it\b/,
  ].some((pattern) => pattern.test(text));
}

const CLOCK_HOURS = ["twelve", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven"];
const CLOCK_SMALL_NUMBERS = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"];
const CLOCK_TENS = { 20: "twenty", 30: "thirty", 40: "forty", 50: "fifty" };

function clockNumber(value) {
  if (value < 20) return CLOCK_SMALL_NUMBERS[value];
  const tens = Math.floor(value / 10) * 10;
  const ones = value % 10;
  return ones ? `${CLOCK_TENS[tens]} ${CLOCK_SMALL_NUMBERS[ones]}` : CLOCK_TENS[tens];
}

export function directClockShortcut(value, date, timeZone = DEFAULT_ASSISTANT_TIME_ZONE) {
  const text = normalizedIntent(value);
  if (!isDirectCurrentTimeIntent(text)) return null;
  const wantsSeconds = /\bseconds?\b/.test(text);
  const now = date instanceof Date ? date : new Date(date);
  if (Number.isNaN(now.getTime())) throw new RangeError("assistant clock returned an invalid date");
  let parts;
  try {
    parts = new Intl.DateTimeFormat("en-US", {
      timeZone, hour: "numeric", minute: "2-digit", second: "2-digit", hour12: true,
    }).formatToParts(now);
  } catch (error) {
    throw new RangeError(`invalid assistant IANA time zone: ${timeZone}`, { cause: error });
  }
  const part = (type) => parts.find((item) => item.type === type)?.value;
  const hour = Number(part("hour"));
  const minute = Number(part("minute"));
  const second = Number(part("second"));
  const period = part("dayPeriod");
  const minuteWords = minute === 0 ? "" : minute < 10 ? ` oh ${clockNumber(minute)}` : ` ${clockNumber(minute)}`;
  const secondsWords = wantsSeconds ? ` and ${clockNumber(second)} ${second === 1 ? "second" : "seconds"}` : "";
  return { route: "local_time", text: `It's ${CLOCK_HOURS[hour % 12]}${minuteWords} ${period}${secondsWords}.` };
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
    /\bwhat did i (?:just )?(?:ask|say)(?: you)?(?: (?:last time|before|previously))?\b/,
    /\bwhat was my (?:last|previous) (?:question|message)\b/,
    /\bwhat did you (?:just )?(?:say|tell)(?: me)?(?: (?:last time|before|previously))?\b/,
    /\bwhat was your (?:last|previous) (?:answer|reply|response)\b/,
  ].some((pattern) => pattern.test(text));
}

function memoryShortcut(value, exchanges) {
  const text = normalizedIntent(value);
  const last = exchanges.at(-1);

  if (/^(what did i (?:just )?(?:ask|say)(?: you)?(?: (?:last time|before|previously))?|what was my (?:last|previous) (?:question|message))$/.test(text)) {
    return last
      ? { route: "memory_user", text: `You just asked: ${last.user}` }
      : { route: "memory_user", text: "I don't have an earlier question in this session." };
  }

  if (/^(what did you (?:just )?(?:say|tell)(?: me)?(?: (?:last time|before|previously))?|what was your (?:last|previous) (?:answer|reply|response))$/.test(text)) {
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

function rawLfmPrompt(messages, reasoningRoute = "THINK", profileRouting = {}) {
  const [system, ...conversation] = messages;
  const turns = conversation.map((message) => `<|im_start|>${message.role}\n${message.content}\n<|im_end|>`).join("\n");
  const mode = reasoningRoute === "FAST" ? profileRouting.fast : profileRouting.think;
  const bridge = mode === "closed_think_bridge" ? "<think>\nNo unnecessary reasoning. Close thinking and answer immediately.\n</think>\n" : "";
  return `<|startoftext|><|im_start|>system\n${system.content}\n<|im_end|>\n${turns}\n<|im_start|>assistant\n${bridge}`;
}

function bareToolPrefixState(value, toolNames = []) {
  const trimmed = String(value ?? "").trimStart();
  if (!trimmed.startsWith("[")) return "none";
  const rest = trimmed.slice(1).trimStart();
  if (!rest) return "maybe";
  for (const name of toolNames) {
    if (name.startsWith(rest)) return "maybe";
    if (!rest.startsWith(name)) continue;
    const suffix = rest.slice(name.length);
    if (!suffix.trim()) return "maybe";
    if (/^\s*\(/.test(suffix)) return "tool";
  }
  return "none";
}

async function readOllamaStream(response, {
  onFirstRawToken, onFirstSpeakableToken, onSpeakableText,
  detectToolCalls = false, toolNames = [],
}) {
  if (!response.body) throw assistantError("assistant_invalid_response");
  const decoder = new TextDecoder();
  const thinkFilter = new ThinkFilter();
  let pending = "";
  let answer = "";
  let reasoningTokens = 0;
  let totalTokens = 0;
  let finalUsage = null;
  let speakableSeen = false;
  let visiblePending = "";
  let toolCallDetected = false;
  let normalTextDetected = !detectToolCalls;
  const emitVisible = (visible, final = false) => {
    if (!visible && !final) return;
    if (normalTextDetected) {
      if (/\S/.test(visible)) { speakableSeen = true; onFirstSpeakableToken(); }
      if (visible) onSpeakableText?.(visible);
      return;
    }
    visiblePending += visible;
    const trimmed = visiblePending.trimStart();
    if (trimmed.startsWith("<|tool")) {
      toolCallDetected = true;
      return;
    }
    if (!final && "<|tool_call_start|>".startsWith(trimmed)) return;
    const bareState = bareToolPrefixState(visiblePending, toolNames);
    if (bareState === "tool") {
      toolCallDetected = true;
      return;
    }
    if (!final && bareState === "maybe") return;
    normalTextDetected = true;
    const ready = visiblePending;
    visiblePending = "";
    if (/\S/.test(ready)) { speakableSeen = true; onFirstSpeakableToken(); }
    if (ready) onSpeakableText?.(ready);
  };
  const consume = (line) => {
    if (!line.trim()) return;
    let payload;
    try { payload = JSON.parse(line); }
    catch { throw assistantError("assistant_invalid_response"); }
    if (payload.error) throw assistantError("assistant_request_failed", String(payload.error));
    if (typeof payload.response === "string" && payload.response.length > 0) {
      totalTokens += 1;
      onFirstRawToken();
      const visible = thinkFilter.push(payload.response);
      if (!/\S/.test(visible) && !speakableSeen) reasoningTokens += 1;
      emitVisible(visible);
      answer += visible;
    }
    if (payload.done) finalUsage = payload;
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
  emitVisible(tail, true);
  return { answer: answer + tail, reasoningTokens,
    totalTokens: Number.isFinite(finalUsage?.eval_count) ? finalUsage.eval_count : totalTokens,
    inputTokens: Number.isFinite(finalUsage?.prompt_eval_count) ? finalUsage.prompt_eval_count : null,
    toolCallDetected };
}

async function readOpenAiStream(response, { onFirstRawToken, onFirstSpeakableToken, onSpeakableText }) {
  if (!response.body) throw assistantError("assistant_invalid_response");
  const decoder = new TextDecoder();
  let pending = "";
  let answer = "";
  const consume = (line) => {
    const trimmed = line.trim();
    if (!trimmed || !trimmed.startsWith("data:")) return;
    const data = trimmed.slice(5).trim();
    if (!data || data === "[DONE]") return;
    let payload;
    try { payload = JSON.parse(data); } catch { throw assistantError("assistant_invalid_response"); }
    if (payload.error) throw assistantError("assistant_request_failed", String(payload.error?.message ?? payload.error));
    const text = payload?.choices?.[0]?.delta?.content;
    if (typeof text !== "string" || !text.length) return;
    onFirstRawToken();
    if (/\S/.test(text)) onFirstSpeakableToken();
    onSpeakableText?.(text);
    answer += text;
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
  fallbackCooldownMs = 30_000, webTools = null, capabilityRouter = null, structuredCapabilities = null,
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
      profile_health: profileHealth,
      active: active.size > 0,
      web_tools: Boolean(webTools?.available && profile?.toolPolicy?.web),
      capability_router: capabilityRouter?.getTelemetry?.() ?? { enabled: false, status: "unconfigured" },
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
    const timer = setTimeout(() => controller.abort(), Math.min(profile.timeoutMs, profile.health.timeoutMs ?? 1_000));
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

  function replaceProfile(profile) {
    if (!profile?.id || !configuredProfiles[profile.id]) throw assistantError("assistant_profile_invalid", String(profile?.id ?? ""));
    configuredProfiles = Object.freeze({ ...configuredProfiles, [profile.id]: profile });
    return getTelemetry();
  }

  function getPreferredProfileConfig() {
    const profile = profileById(preferredId);
    return { id: preferredId, ...profileTuning(profile) };
  }

  async function requestProfile(profile, { deviceId, streamId, transcript, previousExchanges, historyAvailable, generationId,
    onFirstToken, onSpeakableText, onToolStart, onToolEnd, routing, capability, structured }) {
    const endpoint = endpointFor(profile);
    if (!profile?.enabled || !endpoint || !profile.model) return { kind: "unavailable", error: "assistant_unavailable", availabilityFailure: true };
    const controller = new AbortController();
    const startedAt = performance.now();
    let firstRawTokenAt = null;
    let firstTokenAt = null;
    const timer = setTimeout(() => controller.abort(assistantError("assistant_timeout")), profile.timeoutMs);
    active.set(deviceId, { controller, generationId });

    try {
      const usedExchanges = profile.contextPolicy.selectiveHistory && shouldUseHistory(transcript)
        ? previousExchanges.slice(-1) : [];
      const includeTime = profile.contextPolicy.selectiveTimeContext && shouldUseTimeContext(transcript);
      const includeRuntime = profile.contextPolicy.selectiveRuntimeContext && shouldUseRuntimeContext(transcript);
      const stateMessage = includeRuntime ? runtimeContextMessage(runtimeContext?.({ deviceId, streamId })) : null;
      const systemParts = [profile.systemPrompt];
      if (includeTime) systemParts.push(assistantTimeContext(now(), timeZone));
      if (stateMessage) systemParts.push(stateMessage);
      const structuredContext = structuredCapabilityContext(structured);
      if (structuredContext) systemParts.push(structuredContext);
      const toolEnabled = !structuredContext && profile.provider === "ollama_raw" && profile.toolPolicy?.web && webTools?.available;
      const toolDefinitions = toolEnabled ? toolsForCapability(capability, webTools.definitions) : [];
      const toolsExposed = toolDefinitions.map((tool) => tool.name);
      const usableTools = toolDefinitions.length > 0;
      if (usableTools) systemParts.push(
        lfmToolDefinitions(toolDefinitions),
        "Use web_search for explicit web searches and information that may have changed. Use web_read only when the contents of a specific source are needed. For stable facts, answer without tools. Emit only the native tool-call envelope when calling a tool. Never invent current information when a web tool fails.",
      );
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
      let payload = null;
      let rawAnswer = "";
      let round = 0;
      let searchCount = 0;
      let readCount = 0;
      let toolSuccesses = 0;
      let toolAttempts = 0;
      let malformedCalls = 0;
      let lastRoundToolCall = false;
      const toolEvents = [];
      const roundTimings = [];
      const maxRounds = usableTools ? profile.toolPolicy.maxRounds : 1;

      while (round < maxRounds) {
        round += 1;
        lastRoundToolCall = false;
        const roundStartedAt = performance.now();
        let roundFirstRawAt = null;
        let roundFirstSpeakableAt = null;
        logger?.info({ device_id: deviceId, stream_id: streamId, effective_profile: profile.id,
          provider: profile.provider, model: profile.model, llm_round: round }, "Assistant LLM round started");
        const requestBody = profile.provider === "ollama_raw" ? {
          model: profile.model,
          prompt: rawLfmPrompt(messages, routing.route, profile.routing),
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

        let roundUsage = null;
        let toolCallDetected = false;
        if (profile.provider === "ollama_raw") {
          const streamed = await readOllamaStream(response, {
            detectToolCalls: usableTools,
            toolNames: toolDefinitions.map((tool) => tool.name),
            onFirstRawToken: () => {
              roundFirstRawAt ??= performance.now();
              firstRawTokenAt ??= roundFirstRawAt;
            },
            onFirstSpeakableToken: () => {
              roundFirstSpeakableAt ??= performance.now();
              if (firstTokenAt == null) {
                firstTokenAt = roundFirstSpeakableAt;
                onFirstToken?.();
              }
            },
            onSpeakableText,
          });
          rawAnswer = streamed.answer;
          toolCallDetected = streamed.toolCallDetected;
          roundUsage = { prompt_tokens: streamed.inputTokens, completion_tokens: streamed.totalTokens,
            reasoning_tokens: streamed.reasoningTokens };
        } else if (profile.requestOptions.stream && response.headers?.get?.("content-type")?.toLowerCase().includes("text/event-stream")) {
          rawAnswer = await readOpenAiStream(response, {
            onFirstRawToken: () => { roundFirstRawAt ??= performance.now(); firstRawTokenAt ??= roundFirstRawAt; },
            onFirstSpeakableToken: () => {
              roundFirstSpeakableAt ??= performance.now();
              if (firstTokenAt == null) { firstTokenAt = roundFirstSpeakableAt; onFirstToken?.(); }
            },
            onSpeakableText,
          });
        } else {
          try { payload = await response.json(); }
          catch { throw assistantError("assistant_invalid_response"); }
          rawAnswer = payload?.choices?.[0]?.message?.content;
          if (String(rawAnswer ?? "").length > 0) {
            roundFirstRawAt = roundFirstSpeakableAt = performance.now();
            firstRawTokenAt ??= roundFirstRawAt;
            if (firstTokenAt == null) { firstTokenAt = roundFirstSpeakableAt; onFirstToken?.(); }
          }
        }
        const roundCompletedAt = performance.now();
        roundTimings.push({ round,
          first_raw_token_ms: roundFirstRawAt == null ? null : Math.round(roundFirstRawAt - roundStartedAt),
          first_speakable_token_ms: roundFirstSpeakableAt == null ? null : Math.round(roundFirstSpeakableAt - roundStartedAt),
          request_ms: Math.round(roundCompletedAt - roundStartedAt),
          input_tokens: roundUsage?.prompt_tokens ?? null,
          output_tokens: roundUsage?.completion_tokens ?? null,
          reasoning_tokens: roundUsage?.reasoning_tokens ?? null });
        if (roundUsage) payload = { usage: roundUsage };

        if (!usableTools) break;
        let parsed;
        try {
          if (toolCallDetected || String(rawAnswer).includes("<|tool"))
            parsed = parseLfmToolCalls(rawAnswer, { allowBare: toolCallDetected });
          else parsed = { calls: [], content: rawAnswer, protocol: null };
        } catch {
          malformedCalls += 1;
          messages.push({ role: "assistant", content: String(rawAnswer) },
            { role: "tool", content: JSON.stringify([{ ok: false, error: "invalid_tool_call", message: "The tool call was rejected. Use the exact native tool-call format and valid arguments." }]) });
          if (malformedCalls >= 2) throw assistantError("assistant_tool_protocol_invalid");
          continue;
        }
        lastRoundToolCall = parsed.calls.length > 0;
        if (!parsed.calls.length) break;
        if (parsed.content.trim()) throw assistantError("assistant_tool_protocol_invalid");

        const results = [];
        for (const call of parsed.calls) {
          toolAttempts += 1;
          const validation = validateToolCall(call, toolDefinitions);
          let error = validation.ok ? null : validation.error;
          if (!error && call.name === "web_search" && searchCount >= profile.toolPolicy.maxSearches) error = "search_limit_reached";
          if (!error && call.name === "web_read" && readCount >= profile.toolPolicy.maxReads) error = "read_limit_reached";
          if (error) {
            results.push({ tool: call.name, ok: false, error, ...(validation.argument ? { argument: validation.argument } : {}) });
            continue;
          }
          if (call.name === "web_search") searchCount += 1;
          if (call.name === "web_read") readCount += 1;
          const toolStartedAt = performance.now();
          const event = { tool: call.name, round, start_ms: Math.round(toolStartedAt - startedAt), end_ms: null,
            agent_tools_latency_ms: null, provider_latency_ms: null, ok: false };
          toolEvents.push(event);
          logger?.info({ device_id: deviceId, stream_id: streamId, tool_name: call.name,
            llm_round: round, tool_start_ms: event.start_ms }, "Assistant tool started");
          await onToolStart?.({ name: call.name, round });
          try {
            const invoked = await webTools.invoke(call.name, call.arguments, { signal: controller.signal });
            toolSuccesses += 1;
            event.ok = true;
            event.agent_tools_latency_ms = invoked.elapsedMs;
            event.provider_latency_ms = invoked.providerElapsedMs;
            results.push({ tool: call.name, ok: true, result: invoked.value });
          } catch (error) {
            event.error = error?.code ?? "agent_tools_error";
            results.push({ tool: call.name, ok: false, error: event.error,
              message: "Live retrieval failed. Do not invent or imply a verified current answer." });
          } finally {
            event.end_ms = Math.round(performance.now() - startedAt);
            await onToolEnd?.({ name: call.name, round, event });
            logger?.info({ device_id: deviceId, stream_id: streamId, tool_name: call.name,
              llm_round: round, tool_end_ms: event.end_ms, agent_tools_latency_ms: event.agent_tools_latency_ms,
              provider_latency_ms: event.provider_latency_ms, tool_ok: event.ok }, "Assistant tool finished");
          }
        }
        messages.push({ role: "assistant", content: String(rawAnswer) }, { role: "tool", content: JSON.stringify(results) });
      }
      if (usableTools && round >= maxRounds && lastRoundToolCall) throw assistantError("assistant_tool_loop_limit");
      if (toolAttempts > 0 && toolSuccesses === 0) rawAnswer = "I couldn't verify that from live sources right now.";
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
        provider: profile.provider, model: profile.model, reasoning_route: routing.route,
        routing_reasons: routing.reasons, activity: routing.activity,
        llm_rounds: round, llm_round_timings: roundTimings,
        tool_selected: [...(structuredContext ? [structured.tool] : []), ...toolEvents.map((event) => event.tool)],
        tool_events: [...(structuredContext ? [{ tool: structured.tool, provider: structured.provider, structured: true,
          start_ms: null, end_ms: structured.elapsed_ms, provider_latency_ms: structured.elapsed_ms, ok: true }] : []), ...toolEvents],
        tool_attempts: toolAttempts + (structuredContext ? 1 : 0), tool_successes: toolSuccesses + (structuredContext ? 1 : 0),
        agent_tools_latency_ms: toolEvents.reduce((total, event) => total + (event.agent_tools_latency_ms ?? 0), 0) || null,
        provider_latency_ms: (structuredContext ? structured.elapsed_ms : 0) +
          toolEvents.reduce((total, event) => total + (event.provider_latency_ms ?? 0), 0) || null,
        capability_primary: capability?.primary ?? null,
        capability_raw_primary: capability?.raw_primary ?? null,
        capability_abstain: capability?.abstain ?? null,
        capability_confidence: capability?.confidence ?? null,
        capability_margin: capability?.margin ?? null,
        capability_source_need: capability?.source_need ?? null,
        capability_router_latency_ms: capability?.latency_ms ?? null,
        capability_router_request_ms: capability?.request_latency_ms ?? null,
        capability_router_fallback: capability?.fallback ?? true,
        capability_router_reason: capability?.reason ?? null,
        capability_tools_exposed: toolsExposed,
        structured_capability_used: Boolean(structuredContext),
        structured_tool: structured?.tool ?? null,
        structured_provider: structured?.provider ?? null,
        structured_provider_ms: structured?.elapsed_ms ?? null,
        structured_fallback_reason: structuredContext ? null : structured?.reason ?? null,
      };
      if (Number.isFinite(payload?.usage?.prompt_tokens)) timings.input_tokens = payload.usage.prompt_tokens;
      if (Number.isFinite(payload?.usage?.completion_tokens)) timings.output_tokens = payload.usage.completion_tokens;
      if (Number.isFinite(payload?.usage?.reasoning_tokens)) timings.reasoning_tokens = payload.usage.reasoning_tokens;
      logger?.info({ device_id: deviceId, stream_id: streamId, query_text: transcript,
        reply_chars: answer.length, reply_text: answer, time_context: includeTime,
        runtime_context: Boolean(stateMessage), ...timings }, "Assistant text ready");
      return { kind: "response", text: answer, timings, profileId: profile.id, availabilityFailure: false };
    } catch (error) {
      const code = controller.signal.aborted ? controller.signal.reason?.code ?? "assistant_cancelled" : error?.code ?? "assistant_request_failed";
      // Once useful streamed text has started, changing profiles would splice two
      // different answers into one spoken turn. Fallback is only safe pre-output.
      const availabilityFailure = firstTokenAt == null && ["assistant_timeout", "assistant_http_error", "assistant_request_failed"].includes(code);
      if (availabilityFailure) health.set(profile.id, "offline");
      logger?.warn({ device_id: deviceId, stream_id: streamId, preferred_profile: preferredId,
        effective_profile: profile.id, provider: profile.provider, model: profile.model, error_code: code },
      "Assistant LLM request failed");
      return { kind: code === "assistant_timeout" ? "timeout" : "error", error: code, availabilityFailure };
    } finally {
      clearTimeout(timer);
      if (active.get(deviceId)?.controller === controller) active.delete(deviceId);
    }
  }

  async function respond({ deviceId, streamId, text, generationId = 0, onFirstToken, onSpeakableText, onToolStart, onToolEnd }) {
    const transcript = boundedText(text, 800);

    if (!enabled || closing) return { kind: "disabled" };
    if (!transcript) return { kind: "empty" };
    const previousActive = active.get(deviceId);
    if (previousActive) {
      if (generationId > previousActive.generationId) previousActive.controller.abort(assistantError("assistant_cancelled"));
      else return { kind: "busy" };
    }

    const previousExchanges = history.get(deviceId) ?? [];
    const historyAvailable = previousExchanges.length;
    const capability = capabilityRouter?.classify
      ? await capabilityRouter.classify(transcript)
      : { enabled: false, fallback: true, reason: "unconfigured", latency_ms: null, request_latency_ms: null };
    const routing = routeAssistantRequest({ text: transcript, context: { hasHistory: historyAvailable > 0 } });
    const clockShortcut = shouldUseTimeContext(transcript)
      ? directClockShortcut(transcript, now(), timeZone)
      : null;
    const shortcut = clockShortcut ?? memoryShortcut(transcript, previousExchanges);

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
        time_context: shortcut.route === "local_time",
        runtime_context: false,
        preferred_profile: preferredId,
        effective_profile: effectiveId,
        fallback_active: preferredId !== effectiveId,
        fallback_reason: fallbackReason,
        provider: selected?.provider ?? null,
        model: selected?.model ?? null,
        reasoning_route: routing.route,
        routing_reasons: routing.reasons,
        activity: routing.activity,
        reasoning_tokens: 0,
        capability_primary: capability.primary ?? null,
        capability_raw_primary: capability.raw_primary ?? null,
        capability_abstain: capability.abstain ?? null,
        capability_confidence: capability.confidence ?? null,
        capability_margin: capability.margin ?? null,
        capability_source_need: capability.source_need ?? null,
        capability_router_latency_ms: capability.latency_ms ?? null,
        capability_router_request_ms: capability.request_latency_ms ?? null,
        capability_router_fallback: capability.fallback ?? true,
        capability_router_reason: capability.reason ?? null,
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

      onFirstToken?.();
      return { kind: "response", text: answer, timings };
    }

    let structured = { kind: "not_applicable" };
    if (!capability.fallback && !capability.abstain && structuredCapabilities?.supports?.(capability.primary)) {
      const structuredName = capability.primary;
      const controller = new AbortController();
      active.set(deviceId, { controller, generationId });
      try {
        await onToolStart?.({ name: structuredName, capability: structuredName, structured: true });
        structured = await structuredCapabilities.invoke(structuredName, transcript, { signal: controller.signal });
        await onToolEnd?.({ name: structuredName, capability: structuredName, structured: true, event: structured });
      } finally {
        if (active.get(deviceId)?.controller === controller) active.delete(deviceId);
      }
      if (controller.signal.aborted) return { kind: "error", error: controller.signal.reason?.code ?? "assistant_cancelled" };
    }

    if (effectiveId !== preferredId && Date.now() - lastFallbackAt >= fallbackCooldownMs) {
      if (await refreshProfileHealth(preferredId) === "online") recoverPreferred("cooldown_health_check");
      else lastFallbackAt = Date.now();
    }
    let selected = profileById(effectiveId);
    let result = await requestProfile(selected, { deviceId, streamId, transcript, previousExchanges, historyAvailable, generationId, onFirstToken, onToolStart, onToolEnd, routing, capability, structured,
      onSpeakableText: selected.progressiveTts ? (text) => onSpeakableText?.(text, { profileId: selected.id, maxReplyChars: selected.maxReplyChars, chunking: selected.chunking }) : null });
    const fallbackId = selected?.fallbackProfile;
    if (result.availabilityFailure && fallbackId && fallbackId !== selected.id && profileById(fallbackId)?.enabled) {
      fallbackReason = result.error;
      effectiveId = fallbackId;
      lastFallbackAt = Date.now();
      logger?.warn({ preferred_profile: preferredId, failed_profile: selected.id, effective_profile: fallbackId,
        fallback_reason: fallbackReason }, "Assistant profile fallback activated");
      selected = profileById(fallbackId);
      result = await requestProfile(selected, { deviceId, streamId, transcript, previousExchanges, historyAvailable, generationId, onFirstToken, onToolStart, onToolEnd, routing, capability, structured,
        onSpeakableText: selected.progressiveTts ? (text) => onSpeakableText?.(text, { profileId: selected.id, maxReplyChars: selected.maxReplyChars, chunking: selected.chunking }) : null });
    }
    if (result.kind === "response") storeExchange(deviceId, transcript, result.text);
    const { availabilityFailure: _availabilityFailure, profileId: _profileId, ...publicResult } = result;
    return publicResult;
  }

  function cancelDevice(deviceId) {
    active.get(deviceId)?.controller.abort(assistantError("assistant_cancelled"));
  }

  function abortDevice(deviceId) {
    cancelDevice(deviceId);
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
    routeRequest: (input = {}) => {
      const context = { ...(input.context ?? {}) };
      if (!("hasHistory" in context) && input.deviceId) context.hasHistory = (history.get(input.deviceId)?.length ?? 0) > 0;
      const decision = routeAssistantRequest({ ...input, context });
      const profile = profileById(effectiveId);
      return { ...decision, profileId: profile?.id ?? null,
        reasoningMode: profile?.routing?.[decision.route.toLowerCase()] ?? profile?.reasoning ?? "model_default",
        webTools: Boolean(webTools?.available && profile?.toolPolicy?.web) };
    },
    refreshHealth,
    refreshProfileHealth,
    setPreferredProfile,
    replaceProfile,
    getPreferredProfileConfig,
    getTelemetry,
    cancelDevice,
    abortDevice,
    close,
    isActive: (deviceId) => active.has(deviceId),
  };
}
