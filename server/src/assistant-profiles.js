export const QWEN_PROFILE_ID = "qwen3:0.6b";
export const LFM_PROFILE_ID = "lfm2.5:8b";

export const QWEN_SYSTEM_PROMPT = [
  "You are Newo, pronounced Neo, a friendly voice assistant.",
  "Answer the user's latest message directly in natural spoken English.",
  "For general knowledge, give two or three useful factual sentences; for simple questions, one sentence is enough.",
  "In the user's message, I, me, and my mean the user, while you and your mean Neo. In your reply, I, me, and my mean Neo, while you and your mean the user.",
  "If unclear, ask one short clarification. Do not use markdown.",
].join(" ");

export const LFM_SYSTEM_PROMPT = [
  "You are Newo, pronounced Neo, a concise conversational voice assistant.",
  "Refer to your name naturally as Neo; mention the Newo spelling only when asked.",
  "Reply in plain natural language for speech, usually one to three short sentences.",
  "Do not use markdown, reveal reasoning, claim unavailable actions, or continue on your own.",
  "If you cannot do something, say so briefly.",
].join(" ");

const sharedContextPolicy = Object.freeze({
  selectiveHistory: true,
  maxStoredExchanges: 3,
  maxCharsPerMessage: 240,
  selectiveTimeContext: true,
  selectiveRuntimeContext: true,
  deterministicMemoryRoutes: true,
});

export function createAssistantProfiles({ qwenApiKey = null } = {}) {
  return Object.freeze({
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
      reasoning: "no_think_bridge",
      sampling: Object.freeze({ temperature: 0.2, top_k: 80, repeat_penalty: 1.05 }),
      maxOutputTokens: 64,
      maxReplyChars: 300,
      timeoutMs: 15_000,
      keepAlive: -1,
      health: Object.freeze({ method: "ollama_tags", endpoint: "/api/tags", implicitLatestTag: true }),
      contextPolicy: sharedContextPolicy,
      requestOptions: Object.freeze({ raw: true, stream: true }),
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
      sampling: Object.freeze({ temperature: 0.7, top_p: 0.8, top_k: 20, min_p: 0 }),
      maxOutputTokens: 72,
      maxReplyChars: 300,
      timeoutMs: 15_000,
      keepAlive: null,
      health: Object.freeze({ method: "openai_models", endpoint: "/v1/models", implicitLatestTag: false }),
      contextPolicy: sharedContextPolicy,
      requestOptions: Object.freeze({ stream: false }),
      fallbackProfile: null,
    }),
  });
}

export function resolveAssistantProfile(value, profiles = createAssistantProfiles()) {
  const input = String(value ?? "").trim().toLowerCase();
  return Object.values(profiles).find((profile) =>
    profile.id.toLowerCase() === input || profile.aliases.some((alias) => alias.toLowerCase() === input))?.id ?? null;
}
