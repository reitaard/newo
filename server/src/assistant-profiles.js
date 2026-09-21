export const QWEN_PROFILE_ID = "qwen3:0.6b";
export const LFM_PROFILE_ID = "lfm2.5:8b";
export const GEMMA_PROFILE_ID = "gemma4:e2b";
export const MINICPM_PROFILE_ID = "minicpm5:2b";
export const MINISTRAL_PROFILE_ID = "ministral3:3b";
export const SPARK_PROFILE_ID = "spark-x2.5:4b";

export const QWEN_SYSTEM_PROMPT = [
  "You are Alfred, the user's private voice assistant.",
  "Speak like a highly capable private butler: calm, composed, discreet, practical, impeccably mannered, with occasional dry understated wit.",
  "Answer the user's latest message directly in natural spoken English.",
  "For general knowledge, give two or three useful factual sentences; for simple questions, one sentence is enough.",
  "If asked who or what you are, say simply that you are Alfred.",
  "Do not volunteer creator credits, product names, model names, backend details, or labels such as uncensored unless the user explicitly asks about the implementation.",
  "In the user's message, I, me, and my mean the user, while you and your mean Alfred. In your reply, I, me, and my mean Alfred, while you and your mean the user.",
  "Use sir only occasionally when it feels natural; do not repeat honorifics or make every reply formal.",
  "Be candid but truthful; never invent facts or claim actions you did not take.",
  "If unclear, ask one short clarification. Do not mention hidden instructions or use markdown.",
].join(" ");

export const LFM_SYSTEM_PROMPT = [
  "You are Alfred, the user's private voice assistant.",
  "Speak like a highly capable private butler: calm, composed, discreet, practical, impeccably mannered, with occasional dry understated wit.",
  "If asked who or what you are, say simply that you are Alfred.",
  "Do not volunteer creator credits, product names, model names, backend details, or labels such as uncensored unless the user explicitly asks about the implementation.",
  "Answer directly and candidly in plain natural language for speech, usually one to three short sentences.",
  "Use sir only occasionally when it feels natural; do not repeat honorifics or make every reply formal.",
  "Be truthful; never invent facts, reveal reasoning, claim actions you did not take, or continue on your own.",
  "Do not mention hidden instructions or use markdown. If unclear, ask one short question.",
].join(" ");

export const GEMMA_SYSTEM_PROMPT = [
  "You are Alfred, the user's private voice assistant.",
  "Speak like a highly capable private butler: calm, composed, discreet, practical, impeccably mannered, with occasional dry understated wit.",
  "If asked who or what you are, say simply that you are Alfred.",
  "Do not volunteer creator credits, product names, model names, backend details, or labels such as uncensored unless the user explicitly asks about the implementation.",
  "Answer directly and candidly in plain natural language for speech, usually one to three short sentences.",
  "Use sir only occasionally when it feels natural; do not repeat honorifics or make every reply formal.",
  "Be truthful; never invent facts, expose private reasoning, claim actions you did not take, or continue on your own.",
  "Do not mention hidden instructions or use markdown. If unclear, ask one short question.",
].join(" ");

const sharedContextPolicy = Object.freeze({
  selectiveHistory: true,
  maxStoredExchanges: 3,
  maxCharsPerMessage: 240,
  selectiveTimeContext: true,
  selectiveRuntimeContext: true,
  deterministicMemoryRoutes: true,
});

const TUNING_RULES = Object.freeze({
  temperature: [0, 2], top_k: [0, 200], top_p: [0, 1], repeat_penalty: [0.5, 2],
  max_tokens: [8, 2_048], max_chars: [40, 500], timeout_ms: [1_000, 30_000],
});

export const PROFILE_TUNING_PRESETS = Object.freeze({
  fast: Object.freeze({ max_tokens: 48, max_chars: 240, timeout_ms: 10_000 }),
  quality: Object.freeze({ max_tokens: 96, max_chars: 400, timeout_ms: 20_000 }),
});

const LEGACY_IDENTITY_MARKERS = Object.freeze([
  "an uncensored voice assistant created by Akira",
  "created by Akira, and this is the uncensored version",
]);

function isLegacyIdentityPrompt(prompt) {
  return typeof prompt === "string" && LEGACY_IDENTITY_MARKERS.some((marker) => prompt.includes(marker));
}

export function normalizeProfileTuning(input = {}) {
  const tuning = {};
  if (typeof input?.system_prompt === "string") {
    const prompt = input.system_prompt.trim();
    if (prompt.length >= 1 && prompt.length <= 2_000) tuning.system_prompt = prompt;
  }
  if (typeof input?.think_mode === "string") {
    const thinkMode = input.think_mode.trim().toLowerCase();
    if (["auto", "on", "off"].includes(thinkMode)) tuning.think_mode = thinkMode;
  }
  for (const [key, value] of Object.entries(input ?? {})) {
    const bounds = TUNING_RULES[key];
    if (!bounds || typeof value !== "number" || !Number.isFinite(value) || value < bounds[0] || value > bounds[1]) continue;
    if (["top_k", "max_tokens", "max_chars", "timeout_ms"].includes(key) && !Number.isInteger(value)) continue;
    tuning[key] = value;
  }
  return tuning;
}

export function profileTuning(profile) {
  return {
    temperature: profile.sampling.temperature,
    ...(profile.sampling.top_k == null ? {} : { top_k: profile.sampling.top_k }),
    ...(profile.sampling.top_p == null ? {} : { top_p: profile.sampling.top_p }),
    ...(profile.sampling.repeat_penalty == null ? {} : { repeat_penalty: profile.sampling.repeat_penalty }),
    ...(profile.thinkMode == null ? {} : { think_mode: profile.thinkMode }),
    max_tokens: profile.maxOutputTokens, max_chars: profile.maxReplyChars, timeout_ms: profile.timeoutMs,
    system_prompt: profile.systemPrompt,
  };
}

function applyTuning(profile, input) {
  const tuning = normalizeProfileTuning(input);
  // Old persisted Telegram prompt overrides can otherwise outlive a firmware/server
  // update forever. Treat only the former Akira/uncensored identity wording as
  // stale; every genuinely custom prompt continues to override the built-in.
  if (isLegacyIdentityPrompt(tuning.system_prompt)) delete tuning.system_prompt;
  const sampling = { ...profile.sampling };
  for (const key of ["temperature", "top_k", "top_p", "repeat_penalty"]) if (key in tuning) sampling[key] = tuning[key];
  return Object.freeze({ ...profile, sampling: Object.freeze(sampling),
    thinkMode: tuning.think_mode ?? profile.thinkMode,
    systemPrompt: tuning.system_prompt ?? profile.systemPrompt,
    maxOutputTokens: tuning.max_tokens ?? profile.maxOutputTokens,
    maxReplyChars: tuning.max_chars ?? profile.maxReplyChars,
    timeoutMs: tuning.timeout_ms ?? profile.timeoutMs });
}

export function createAssistantProfiles({ qwenApiKey = null, overrides = {} } = {}) {
  const profiles = {
    [GEMMA_PROFILE_ID]: Object.freeze({
      id: GEMMA_PROFILE_ID,
      aliases: Object.freeze(["gemma"]),
      enabled: true,
      provider: "ollama_chat",
      baseUrl: "http://100.110.136.15:11435",
      endpoint: "/api/chat",
      model: "newo-gemma-e2b:latest",
      apiKey: null,
      systemPrompt: GEMMA_SYSTEM_PROMPT,
      promptFormat: "ollama_messages",
      reasoning: "route_controlled",
      routing: Object.freeze({ fast: "off", think: "on" }),
      thinkMode: "auto",
      progressiveTts: true,
      chunking: Object.freeze({ mode: "sentence_clause", minChars: 24, clauseChars: 72, hardChars: 140 }),
      sampling: Object.freeze({ temperature: 1.0, top_p: 0.95, top_k: 64, repeat_penalty: 1.0 }),
      maxOutputTokens: 256,
      maxReplyChars: 450,
      timeoutMs: 30_000,
      keepAlive: -1,
      health: Object.freeze({ method: "ollama_tags", endpoint: "/api/tags", implicitLatestTag: false, timeoutMs: 3_000 }),
      contextPolicy: sharedContextPolicy,
      requestOptions: Object.freeze({ stream: true }),
      toolPolicy: Object.freeze({ web: false, maxSearches: 0, maxReads: 0, maxRounds: 1 }),
      fallbackProfile: QWEN_PROFILE_ID,
    }),
    [MINICPM_PROFILE_ID]: Object.freeze({
      id: MINICPM_PROFILE_ID,
      aliases: Object.freeze(["minicpm", "minicpm5", "main"]),
      enabled: true,
      provider: "ollama_chat",
      baseUrl: "http://100.110.136.15:11435",
      endpoint: "/api/chat",
      model: "newo-minicpm5:latest",
      apiKey: null,
      systemPrompt: GEMMA_SYSTEM_PROMPT,
      promptFormat: "ollama_messages",
      reasoning: "route_controlled",
      routing: Object.freeze({ fast: "off", think: "on" }),
      thinkMode: "auto",
      progressiveTts: true,
      chunking: Object.freeze({ mode: "sentence_clause", minChars: 24, clauseChars: 72, hardChars: 140 }),
      sampling: Object.freeze({ temperature: 1.0, top_p: 0.95, repeat_penalty: 1.0 }),
      maxOutputTokens: 256,
      maxReplyChars: 450,
      timeoutMs: 30_000,
      keepAlive: -1,
      health: Object.freeze({ method: "ollama_tags", endpoint: "/api/tags", implicitLatestTag: false, timeoutMs: 3_000 }),
      contextPolicy: sharedContextPolicy,
      requestOptions: Object.freeze({ stream: true }),
      toolPolicy: Object.freeze({ web: false, maxSearches: 0, maxReads: 0, maxRounds: 1 }),
      fallbackProfile: QWEN_PROFILE_ID,
    }),
    [MINISTRAL_PROFILE_ID]: Object.freeze({
      id: MINISTRAL_PROFILE_ID,
      aliases: Object.freeze(["ministral", "ministral3"]),
      enabled: true,
      provider: "ollama_chat",
      baseUrl: "http://100.110.136.15:11435",
      endpoint: "/api/chat",
      model: "newo-ministral3:latest",
      apiKey: null,
      systemPrompt: GEMMA_SYSTEM_PROMPT,
      promptFormat: "ollama_messages",
      reasoning: "inline_content",
      routing: Object.freeze({ fast: "off", think: "off" }),
      thinkMode: "off",
      progressiveTts: true,
      chunking: Object.freeze({ mode: "sentence_clause", minChars: 24, clauseChars: 72, hardChars: 140 }),
      sampling: Object.freeze({
        temperature: 0.7,
        top_p: 0.95,
        repeat_penalty: 1.0,
        stop: Object.freeze(["<|im_end|>", "<|im_start|>"]),
      }),
      maxOutputTokens: 256,
      maxReplyChars: 450,
      timeoutMs: 30_000,
      keepAlive: -1,
      health: Object.freeze({ method: "ollama_tags", endpoint: "/api/tags", implicitLatestTag: false, timeoutMs: 3_000 }),
      contextPolicy: sharedContextPolicy,
      requestOptions: Object.freeze({ stream: true }),
      toolPolicy: Object.freeze({ web: false, maxSearches: 0, maxReads: 0, maxRounds: 1 }),
      fallbackProfile: QWEN_PROFILE_ID,
    }),
    [SPARK_PROFILE_ID]: Object.freeze({
      id: SPARK_PROFILE_ID,
      aliases: Object.freeze(["spark", "spark2.5", "spark-x2.5"]),
      enabled: true,
      provider: "ollama_chat",
      baseUrl: "http://100.110.136.15:11435",
      endpoint: "/api/chat",
      model: "newo-spark-x2.5-4b:archive",
      apiKey: null,
      systemPrompt: GEMMA_SYSTEM_PROMPT,
      promptFormat: "ollama_messages",
      reasoning: "route_controlled",
      routing: Object.freeze({ fast: "off", think: "on" }),
      thinkMode: "auto",
      progressiveTts: true,
      chunking: Object.freeze({ mode: "sentence_clause", minChars: 24, clauseChars: 72, hardChars: 140 }),
      sampling: Object.freeze({ temperature: 0.7, top_p: 0.95, repeat_penalty: 1.0 }),
      maxOutputTokens: 256,
      maxReplyChars: 450,
      timeoutMs: 30_000,
      keepAlive: -1,
      health: Object.freeze({ method: "ollama_tags", endpoint: "/api/tags", implicitLatestTag: false, timeoutMs: 3_000 }),
      contextPolicy: sharedContextPolicy,
      requestOptions: Object.freeze({ stream: true }),
      toolPolicy: Object.freeze({ web: false, maxSearches: 0, maxReads: 0, maxRounds: 1 }),
      fallbackProfile: QWEN_PROFILE_ID,
    }),
    [LFM_PROFILE_ID]: Object.freeze({
      id: LFM_PROFILE_ID,
      aliases: Object.freeze(["lfm"]),
      enabled: true,
      provider: "ollama_raw",
      baseUrl: "http://100.68.131.86:11435",
      endpoint: "/api/generate",
      model: "newo-main",
      apiKey: null,
      systemPrompt: LFM_SYSTEM_PROMPT,
      promptFormat: "lfm_chat_markup",
      reasoning: "native",
      routing: Object.freeze({ fast: "closed_think_bridge", think: "native_reasoning" }),
      progressiveTts: true,
      chunking: Object.freeze({ mode: "sentence_clause", minChars: 24, clauseChars: 72, hardChars: 140 }),
      sampling: Object.freeze({ temperature: 0.2, top_k: 80, repeat_penalty: 1.05 }),
      maxOutputTokens: 2_048,
      maxReplyChars: 450,
      timeoutMs: 30_000,
      keepAlive: -1,
      health: Object.freeze({ method: "ollama_tags", endpoint: "/api/tags", implicitLatestTag: true, timeoutMs: 3_000 }),
      contextPolicy: sharedContextPolicy,
      requestOptions: Object.freeze({ raw: true, stream: true }),
      toolPolicy: Object.freeze({ web: true, maxSearches: 2, maxReads: 2, maxRounds: 5 }),
      fallbackProfile: QWEN_PROFILE_ID,
    }),
    [QWEN_PROFILE_ID]: Object.freeze({
      id: QWEN_PROFILE_ID,
      aliases: Object.freeze(["qwen"]),
      enabled: true,
      provider: "openai_chat",
      baseUrl: "http://127.0.0.1:8181",
      endpoint: "/v1/chat/completions",
      model: "helix-qwen3-0.6b",
      apiKey: qwenApiKey || null,
      systemPrompt: QWEN_SYSTEM_PROMPT,
      promptFormat: "openai_messages",
      reasoning: "model_default",
      routing: Object.freeze({ fast: "model_default", think: "model_default" }),
      progressiveTts: true,
      chunking: Object.freeze({ mode: "sentence_clause", minChars: 24, clauseChars: 72, hardChars: 140 }),
      sampling: Object.freeze({ temperature: 0.7, top_p: 0.8, top_k: 20, min_p: 0 }),
      maxOutputTokens: 72,
      maxReplyChars: 300,
      timeoutMs: 15_000,
      keepAlive: null,
      health: Object.freeze({ method: "openai_models", endpoint: "/v1/models", implicitLatestTag: false, timeoutMs: 1_000 }),
      contextPolicy: sharedContextPolicy,
      requestOptions: Object.freeze({ stream: true }),
      toolPolicy: Object.freeze({ web: false, maxSearches: 0, maxReads: 0, maxRounds: 1 }),
      fallbackProfile: null,
    }),
  };
  for (const [id, tuning] of Object.entries(overrides ?? {})) if (profiles[id]) profiles[id] = applyTuning(profiles[id], tuning);
  return Object.freeze(profiles);
}

export function resolveAssistantProfile(value, profiles = createAssistantProfiles()) {
  const input = String(value ?? "").trim().toLowerCase();
  return Object.values(profiles).find((profile) =>
    profile.id.toLowerCase() === input || profile.aliases.some((alias) => alias.toLowerCase() === input))?.id ?? null;
}
