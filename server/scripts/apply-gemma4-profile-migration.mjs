import { readFile, writeFile } from "node:fs/promises";

const files = {
  profiles: new URL("../src/assistant-profiles.js", import.meta.url),
  assistant: new URL("../src/assistant.js", import.meta.url),
  telegram: new URL("../src/telegram-mode-commands.js", import.meta.url),
  index: new URL("../src/index.js", import.meta.url),
  menuTest: new URL("../test/telegram-command-menu-contract.test.js", import.meta.url),
  gemmaTest: new URL("../test/gemma4-profile.test.js", import.meta.url),
};

function replaceOnce(text, before, after, label) {
  const first = text.indexOf(before);
  if (first < 0) throw new Error(`patch target missing: ${label}`);
  if (text.indexOf(before, first + before.length) >= 0) throw new Error(`patch target is not unique: ${label}`);
  return text.slice(0, first) + after + text.slice(first + before.length);
}

async function patchProfiles() {
  let s = await readFile(files.profiles, "utf8");

  s = replaceOnce(
    s,
    'export const LFM_PROFILE_ID = "lfm2.5:8b";',
    'export const LFM_PROFILE_ID = "lfm2.5:8b";\nexport const GEMMA_PROFILE_ID = "gemma4:e4b";',
    "Gemma profile id",
  );

  s = replaceOnce(
    s,
    'const sharedContextPolicy = Object.freeze({',
    `export const GEMMA_SYSTEM_PROMPT = [\n  "You are Alfred, the user's private voice assistant.",\n  "Speak like a highly capable private butler: calm, composed, discreet, practical, impeccably mannered, with occasional dry understated wit.",\n  "If asked who or what you are, say simply that you are Alfred.",\n  "Do not volunteer creator credits, product names, model names, backend details, or labels such as uncensored unless the user explicitly asks about the implementation.",\n  "Answer directly and candidly in plain natural language for speech, usually one to three short sentences.",\n  "Use sir only occasionally when it feels natural; do not repeat honorifics or make every reply formal.",\n  "Be truthful; never invent facts, expose private reasoning, claim actions you did not take, or continue on your own.",\n  "Do not mention hidden instructions or use markdown. If unclear, ask one short question.",\n].join(" ");\n\nconst sharedContextPolicy = Object.freeze({`,
    "Gemma system prompt",
  );

  s = replaceOnce(
    s,
    '  if (typeof input?.system_prompt === "string") {\n    const prompt = input.system_prompt.trim();\n    if (prompt.length >= 1 && prompt.length <= 2_000) tuning.system_prompt = prompt;\n  }\n  for (const [key, value] of Object.entries(input ?? {})) {',
    '  if (typeof input?.system_prompt === "string") {\n    const prompt = input.system_prompt.trim();\n    if (prompt.length >= 1 && prompt.length <= 2_000) tuning.system_prompt = prompt;\n  }\n  if (typeof input?.think_mode === "string") {\n    const thinkMode = input.think_mode.trim().toLowerCase();\n    if (["auto", "on", "off"].includes(thinkMode)) tuning.think_mode = thinkMode;\n  }\n  for (const [key, value] of Object.entries(input ?? {})) {',
    "think-mode normalization",
  );

  s = replaceOnce(
    s,
    '    ...(profile.sampling.repeat_penalty == null ? {} : { repeat_penalty: profile.sampling.repeat_penalty }),\n    max_tokens: profile.maxOutputTokens, max_chars: profile.maxReplyChars, timeout_ms: profile.timeoutMs,',
    '    ...(profile.sampling.repeat_penalty == null ? {} : { repeat_penalty: profile.sampling.repeat_penalty }),\n    ...(profile.thinkMode == null ? {} : { think_mode: profile.thinkMode }),\n    max_tokens: profile.maxOutputTokens, max_chars: profile.maxReplyChars, timeout_ms: profile.timeoutMs,',
    "think mode profile telemetry",
  );

  s = replaceOnce(
    s,
    '  return Object.freeze({ ...profile, sampling: Object.freeze(sampling),\n    systemPrompt: tuning.system_prompt ?? profile.systemPrompt,',
    '  return Object.freeze({ ...profile, sampling: Object.freeze(sampling),\n    thinkMode: tuning.think_mode ?? profile.thinkMode,\n    systemPrompt: tuning.system_prompt ?? profile.systemPrompt,',
    "think mode profile override",
  );

  s = replaceOnce(
    s,
    '  const profiles = {\n    [LFM_PROFILE_ID]: Object.freeze({',
    `  const profiles = {\n    [GEMMA_PROFILE_ID]: Object.freeze({\n      id: GEMMA_PROFILE_ID,\n      aliases: Object.freeze(["gemma"]),\n      enabled: true,\n      provider: "ollama_chat",\n      baseUrl: "http://100.110.136.15:11435",\n      endpoint: "/api/chat",\n      model: "hf.co/HauhauCS/Gemma-4-E4B-Uncensored-HauhauCS-Aggressive:Q4_K_M",\n      apiKey: null,\n      systemPrompt: GEMMA_SYSTEM_PROMPT,\n      promptFormat: "ollama_messages",\n      reasoning: "route_controlled",\n      routing: Object.freeze({ fast: "off", think: "on" }),\n      thinkMode: "auto",\n      progressiveTts: true,\n      chunking: Object.freeze({ mode: "sentence_clause", minChars: 24, clauseChars: 72, hardChars: 140 }),\n      sampling: Object.freeze({ temperature: 1.0, top_p: 0.95, top_k: 64, repeat_penalty: 1.0 }),\n      maxOutputTokens: 256,\n      maxReplyChars: 450,\n      timeoutMs: 30_000,\n      keepAlive: -1,\n      health: Object.freeze({ method: "ollama_tags", endpoint: "/api/tags", implicitLatestTag: false, timeoutMs: 3_000 }),\n      contextPolicy: sharedContextPolicy,\n      requestOptions: Object.freeze({ stream: true }),\n      toolPolicy: Object.freeze({ web: false, maxSearches: 0, maxReads: 0, maxRounds: 1 }),\n      fallbackProfile: QWEN_PROFILE_ID,\n    }),\n    [LFM_PROFILE_ID]: Object.freeze({`,
    "Gemma profile",
  );

  await writeFile(files.profiles, s);
}

async function patchAssistant() {
  let s = await readFile(files.assistant, "utf8");

  s = replaceOnce(
    s,
    '  createAssistantProfiles,\n  LFM_PROFILE_ID,',
    '  createAssistantProfiles,\n  GEMMA_PROFILE_ID,\n  LFM_PROFILE_ID,',
    "Gemma assistant import",
  );

  s = replaceOnce(
    s,
    'async function readOpenAiStream(response, { onFirstRawToken, onFirstSpeakableToken, onSpeakableText }) {',
    `function resolveOllamaThink(profile, routing) {\n  if (profile.thinkMode === "on") return true;\n  if (profile.thinkMode === "off") return false;\n  return routing?.route === "THINK";\n}\n\nasync function readOllamaChatStream(response, { onFirstRawToken, onFirstSpeakableToken, onSpeakableText }) {\n  if (!response.body) throw assistantError("assistant_invalid_response");\n  const decoder = new TextDecoder();\n  let pending = "";\n  let answer = "";\n  let finalUsage = null;\n  let thinkingSeen = false;\n  const consume = (line) => {\n    if (!line.trim()) return;\n    let payload;\n    try { payload = JSON.parse(line); }\n    catch { throw assistantError("assistant_invalid_response"); }\n    if (payload.error) throw assistantError("assistant_request_failed", String(payload.error));\n    const thinking = payload?.message?.thinking;\n    const text = payload?.message?.content;\n    if (typeof thinking === "string" && thinking.length > 0) {\n      thinkingSeen = true;\n      onFirstRawToken();\n    }\n    if (typeof text === "string" && text.length > 0) {\n      onFirstRawToken();\n      if (/\\S/.test(text)) onFirstSpeakableToken();\n      onSpeakableText?.(text);\n      answer += text;\n    }\n    if (payload.done) finalUsage = payload;\n  };\n  for await (const chunk of response.body) {\n    pending += decoder.decode(chunk, { stream: true });\n    const lines = pending.split(/\\r?\\n/);\n    pending = lines.pop() ?? "";\n    for (const line of lines) consume(line);\n  }\n  pending += decoder.decode();\n  if (pending.trim()) consume(pending);\n  return {\n    answer,\n    inputTokens: Number.isFinite(finalUsage?.prompt_eval_count) ? finalUsage.prompt_eval_count : null,\n    totalTokens: Number.isFinite(finalUsage?.eval_count) ? finalUsage.eval_count : null,\n    reasoningTokens: thinkingSeen ? null : 0,\n  };\n}\n\nasync function readOpenAiStream(response, { onFirstRawToken, onFirstSpeakableToken, onSpeakableText }) {`,
    "native Ollama chat stream reader",
  );

  s = replaceOnce(
    s,
    '  const requestedId = resolveAssistantProfile(preferredProfile, configuredProfiles) ??\n    (provider === "ollama_raw" ? LFM_PROFILE_ID : QWEN_PROFILE_ID);',
    '  const requestedId = resolveAssistantProfile(preferredProfile, configuredProfiles) ??\n    (provider === "ollama_chat" ? GEMMA_PROFILE_ID : provider === "ollama_raw" ? LFM_PROFILE_ID : QWEN_PROFILE_ID);',
    "provider default profile",
  );

  s = replaceOnce(
    s,
    '      const models = profile.provider === "ollama_raw" ? payload?.models : payload?.data;\n      const online = Array.isArray(models) && models.some((item) => {\n        const candidate = profile.provider === "ollama_raw" ? item?.name ?? item?.model : item?.id;',
    '      const ollamaProvider = profile.provider === "ollama_raw" || profile.provider === "ollama_chat";\n      const models = ollamaProvider ? payload?.models : payload?.data;\n      const online = Array.isArray(models) && models.some((item) => {\n        const candidate = ollamaProvider ? item?.name ?? item?.model : item?.id;',
    "Ollama chat health check",
  );

  s = replaceOnce(
    s,
    '        const requestBody = profile.provider === "ollama_raw" ? {\n          model: profile.model,\n          prompt: rawLfmPrompt(messages, routing.route, profile.routing),\n          raw: profile.requestOptions.raw,\n          stream: profile.requestOptions.stream,\n          keep_alive: profile.keepAlive,\n          options: {\n            num_predict: profile.maxOutputTokens,\n            ...profile.sampling,\n            stop: ["<|im_end|>", "<|im_start|>"],\n          },\n        } : {\n          model: profile.model,\n          messages,\n          max_tokens: profile.maxOutputTokens,\n          ...profile.sampling,\n          stream: profile.requestOptions.stream,\n        };',
    '        const requestBody = profile.provider === "ollama_raw" ? {\n          model: profile.model,\n          prompt: rawLfmPrompt(messages, routing.route, profile.routing),\n          raw: profile.requestOptions.raw,\n          stream: profile.requestOptions.stream,\n          keep_alive: profile.keepAlive,\n          options: {\n            num_predict: profile.maxOutputTokens,\n            ...profile.sampling,\n            stop: ["<|im_end|>", "<|im_start|>"],\n          },\n        } : profile.provider === "ollama_chat" ? {\n          model: profile.model,\n          messages,\n          think: resolveOllamaThink(profile, routing),\n          stream: profile.requestOptions.stream,\n          keep_alive: profile.keepAlive,\n          options: {\n            num_predict: profile.maxOutputTokens,\n            ...profile.sampling,\n          },\n        } : {\n          model: profile.model,\n          messages,\n          max_tokens: profile.maxOutputTokens,\n          ...profile.sampling,\n          stream: profile.requestOptions.stream,\n        };',
    "Ollama chat request body",
  );

  s = replaceOnce(
    s,
    '        if (profile.provider === "ollama_raw") {\n          const streamed = await readOllamaStream(response, {',
    '        if (profile.provider === "ollama_chat") {\n          const streamed = await readOllamaChatStream(response, {\n            onFirstRawToken: () => {\n              roundFirstRawAt ??= performance.now();\n              firstRawTokenAt ??= roundFirstRawAt;\n            },\n            onFirstSpeakableToken: () => {\n              roundFirstSpeakableAt ??= performance.now();\n              if (firstTokenAt == null) {\n                firstTokenAt = roundFirstSpeakableAt;\n                onFirstToken?.();\n              }\n            },\n            onSpeakableText,\n          });\n          rawAnswer = streamed.answer;\n          roundUsage = { prompt_tokens: streamed.inputTokens, completion_tokens: streamed.totalTokens,\n            reasoning_tokens: streamed.reasoningTokens };\n        } else if (profile.provider === "ollama_raw") {\n          const streamed = await readOllamaStream(response, {',
    "Ollama chat response branch",
  );

  s = replaceOnce(
    s,
    '        provider: profile.provider, model: profile.model, reasoning_route: routing.route,\n        routing_reasons: routing.reasons, activity: routing.activity,',
    '        provider: profile.provider, model: profile.model, reasoning_route: routing.route,\n        think_mode: profile.thinkMode ?? null,\n        think_enabled: profile.provider === "ollama_chat" ? resolveOllamaThink(profile, routing) : null,\n        routing_reasons: routing.reasons, activity: routing.activity,',
    "Gemma thinking telemetry",
  );

  s = replaceOnce(
    s,
    '      return { ...decision, profileId: profile?.id ?? null,\n        reasoningMode: profile?.routing?.[decision.route.toLowerCase()] ?? profile?.reasoning ?? "model_default",',
    '      return { ...decision, profileId: profile?.id ?? null,\n        reasoningMode: profile?.provider === "ollama_chat"\n          ? (profile.thinkMode === "auto" ? (decision.route === "THINK" ? "on" : "off") : profile.thinkMode)\n          : profile?.routing?.[decision.route.toLowerCase()] ?? profile?.reasoning ?? "model_default",',
    "route think telemetry",
  );

  await writeFile(files.assistant, s);
}

async function patchTelegram() {
  let s = await readFile(files.telegram, "utf8");

  s = replaceOnce(
    s,
    '    `Repeat penalty: ${bold(tuning.repeat_penalty ?? "n/a")}`,\n    `Output: ${bold(`${tuning.max_tokens ?? "n/a"} tokens / ${tuning.max_chars ?? "n/a"} chars`)}`,',
    '    `Repeat penalty: ${bold(tuning.repeat_penalty ?? "n/a")}`,\n    `Think: ${bold(String(tuning.think_mode ?? "n/a").toUpperCase())}`,\n    `Output: ${bold(`${tuning.max_tokens ?? "n/a"} tokens / ${tuning.max_chars ?? "n/a"} chars`)}`,',
    "Telegram think status",
  );

  s = replaceOnce(
    s,
    '    const setting = requested.match(/^(?:set|s)\\s+(topk|topp|maxtoken|maxchars|timeout|rpenalty|temp)\\s+([^\\s]+)$/i);',
    '    const setting = requested.match(/^(?:set|s)\\s+(topk|topp|maxtoken|maxchars|timeout|rpenalty|temp|think)\\s+([^\\s]+)$/i);',
    "Telegram think setter",
  );

  s = replaceOnce(
    s,
    '["Usage: /profile [lfm|qwen]"]',
    '["Usage: /profile [gemma|qwen]"]',
    "Telegram profile usage",
  );

  await writeFile(files.telegram, s);
}

async function patchIndex() {
  let s = await readFile(files.index, "utf8");

  s = replaceOnce(
    s,
    'import { createAssistantProfiles, normalizeProfileTuning, PROFILE_TUNING_PRESETS, QWEN_PROFILE_ID, resolveAssistantProfile } from "./assistant-profiles.js";',
    'import { createAssistantProfiles, GEMMA_PROFILE_ID, LFM_PROFILE_ID, normalizeProfileTuning, PROFILE_TUNING_PRESETS, QWEN_PROFILE_ID, resolveAssistantProfile } from "./assistant-profiles.js";',
    "Gemma index imports",
  );

  s = replaceOnce(
    s,
    'ASSISTANT_PROVIDER: z.preprocess(emptyToUndefined, z.enum(["openai_chat", "ollama_raw"]).default("openai_chat")),',
    'ASSISTANT_PROVIDER: z.preprocess(emptyToUndefined, z.enum(["openai_chat", "ollama_raw", "ollama_chat"]).default("openai_chat")),',
    "ollama_chat environment provider",
  );

  s = replaceOnce(
    s,
    '  preferredProfile: resolveAssistantProfile(runtimeState.assistantProfile, assistantProfiles) ?? configuredAssistantProfile,',
    '  preferredProfile: (resolveAssistantProfile(runtimeState.assistantProfile, assistantProfiles) === LFM_PROFILE_ID && configuredAssistantProfile === GEMMA_PROFILE_ID)\n    ? GEMMA_PROFILE_ID\n    : resolveAssistantProfile(runtimeState.assistantProfile, assistantProfiles) ?? configuredAssistantProfile,',
    "migrate persisted LFM preference when Gemma is configured",
  );

  s = replaceOnce(
    s,
    '  { command: "profile_lfm", description: "Use LFM assistant" },\n  { command: "profile_qwen", description: "Use Qwen assistant" },',
    '  { command: "profile_gemma", description: "Use Gemma assistant" },\n  { command: "profile_qwen", description: "Use Qwen assistant" },',
    "Telegram Gemma menu",
  );

  s = replaceOnce(
    s,
    '  bot.command(["profile_lfm", "p_lfm"], (ctx) => primaryModeHandlers.profile(ctx, "lfm"));\n  bot.command(["profile_qwen", "p_qwen"], (ctx) => primaryModeHandlers.profile(ctx, "qwen"));',
    '  bot.command(["profile_gemma", "p_gemma"], (ctx) => primaryModeHandlers.profile(ctx, "gemma"));\n  // Hidden compatibility command during migration; it is not advertised in the Telegram menu.\n  bot.command(["profile_lfm", "p_lfm"], (ctx) => primaryModeHandlers.profile(ctx, "lfm"));\n  bot.command(["profile_qwen", "p_qwen"], (ctx) => primaryModeHandlers.profile(ctx, "qwen"));',
    "Telegram Gemma command",
  );

  s = replaceOnce(
    s,
    '    if (preset === "reset" || preset === "balanced") delete nextOverrides[id];\n    else if (PROFILE_TUNING_PRESETS[preset]) nextOverrides[id] = PROFILE_TUNING_PRESETS[preset];\n    else throw new Error("invalid profile tuning preset");',
    '    if (preset === "reset" || preset === "balanced") delete nextOverrides[id];\n    else if (PROFILE_TUNING_PRESETS[preset]) {\n      const basePreset = PROFILE_TUNING_PRESETS[preset];\n      nextOverrides[id] = id === GEMMA_PROFILE_ID\n        ? { ...basePreset, think_mode: preset === "fast" ? "off" : "on" }\n        : basePreset;\n    } else throw new Error("invalid profile tuning preset");',
    "Gemma think presets",
  );

  s = replaceOnce(
    s,
    '    const canonical = { topk: "top_k", topp: "top_p", maxtoken: "max_tokens", maxchars: "max_chars", timeout: "timeout_ms", rpenalty: "repeat_penalty", temp: "temperature" }[key];\n    const tuning = normalizeProfileTuning({ [canonical]: Number(value) });\n    if (!canonical || !Object.hasOwn(tuning, canonical)) throw new Error("invalid profile tuning value");',
    '    const canonical = { topk: "top_k", topp: "top_p", maxtoken: "max_tokens", maxchars: "max_chars", timeout: "timeout_ms", rpenalty: "repeat_penalty", temp: "temperature", think: "think_mode" }[key];\n    if (!canonical) throw new Error("invalid profile tuning value");\n    if (canonical === "think_mode" && id !== GEMMA_PROFILE_ID) throw new Error("thinking control is only available for Gemma");\n    const parsedValue = canonical === "think_mode" ? String(value).trim().toLowerCase() : Number(value);\n    const tuning = normalizeProfileTuning({ [canonical]: parsedValue });\n    if (!Object.hasOwn(tuning, canonical)) throw new Error("invalid profile tuning value");',
    "Gemma think setting persistence",
  );

  await writeFile(files.index, s);
}

async function patchMenuTest() {
  let s = await readFile(files.menuTest, "utf8");
  s = s.replace('"eco", "clock", "track", "track_bg", "voice", "profile", "profile_lfm", "profile_qwen", "speaker", "ping",',
    '"eco", "clock", "track", "track_bg", "voice", "profile", "profile_gemma", "profile_qwen", "speaker", "ping",');
  s = s.replace('assert.match(server, /bot\\.command\\(\\["profile_lfm", "p_lfm"\\]/);',
    'assert.match(server, /bot\\.command\\(\\["profile_gemma", "p_gemma"\\]/);\n  assert.match(server, /bot\\.command\\(\\["profile_lfm", "p_lfm"\\]/);');
  await writeFile(files.menuTest, s);
}

async function writeGemmaTests() {
  const test = `import assert from "node:assert/strict";\nimport test from "node:test";\n\nimport { createAssistantRuntime } from "../src/assistant.js";\nimport { createAssistantProfiles, GEMMA_PROFILE_ID, QWEN_PROFILE_ID, resolveAssistantProfile } from "../src/assistant-profiles.js";\n\nconst quietLogger = { info() {}, warn() {} };\nconst turn = { deviceId: "newo-01", streamId: "gemma-1", text: "hello" };\n\nfunction jsonResponse(payload, status = 200) {\n  return new Response(JSON.stringify(payload), { status, headers: { "content-type": "application/json" } });\n}\n\nfunction ndjsonResponse(parts, signal) {\n  return new Response(new ReadableStream({\n    start(controller) {\n      signal?.addEventListener("abort", () => controller.error(signal.reason), { once: true });\n      for (const part of parts) controller.enqueue(new TextEncoder().encode(JSON.stringify(part) + "\\n"));\n      if (!signal?.aborted) controller.close();\n    },\n  }), { status: 200, headers: { "content-type": "application/x-ndjson" } });\n}\n\nfunction gemmaProfiles(overrides = {}) {\n  const profiles = createAssistantProfiles({ overrides });\n  return {\n    ...profiles,\n    [GEMMA_PROFILE_ID]: { ...profiles[GEMMA_PROFILE_ID], baseUrl: "http://gemma.test" },\n    [QWEN_PROFILE_ID]: { ...profiles[QWEN_PROFILE_ID], baseUrl: "http://qwen.test" },\n  };\n}\n\ntest("Gemma profile uses persistent laptop endpoint and native Ollama chat", () => {\n  const profile = createAssistantProfiles()[GEMMA_PROFILE_ID];\n  assert.equal(resolveAssistantProfile("gemma"), GEMMA_PROFILE_ID);\n  assert.equal(profile.provider, "ollama_chat");\n  assert.equal(profile.baseUrl, "http://100.110.136.15:11435");\n  assert.equal(profile.endpoint, "/api/chat");\n  assert.equal(profile.model, "hf.co/HauhauCS/Gemma-4-E4B-Uncensored-HauhauCS-Aggressive:Q4_K_M");\n  assert.equal(profile.thinkMode, "auto");\n  assert.equal(profile.fallbackProfile, QWEN_PROFILE_ID);\n});\n\ntest("Gemma FAST sends think false and never speaks message.thinking", async () => {\n  let body;\n  const runtime = createAssistantRuntime({ enabled: true, profiles: gemmaProfiles(), preferredProfile: "gemma", logger: quietLogger,\n    fetchImpl: async (url, options = {}) => {\n      assert.equal(url, "http://gemma.test/api/chat");\n      body = JSON.parse(options.body);\n      return ndjsonResponse([\n        { message: { role: "assistant", thinking: "private chain" }, done: false },\n        { message: { role: "assistant", content: "Visible answer." }, done: false },\n        { message: { role: "assistant", content: "" }, done: true, prompt_eval_count: 20, eval_count: 9 },\n      ], options.signal);\n    } });\n  const spoken = [];\n  const result = await runtime.respond({ ...turn }, { onSpeakableText: (text) => spoken.push(text) });\n  assert.equal(result.kind, "response");\n  assert.equal(result.text, "Visible answer.");\n  assert.equal(body.think, false);\n  assert.ok(Array.isArray(body.messages));\n  assert.deepEqual(spoken, ["Visible answer."]);\n});\n\ntest("Gemma THINK route enables native thinking while content remains separate", async () => {\n  let body;\n  const runtime = createAssistantRuntime({ enabled: true, profiles: gemmaProfiles(), preferredProfile: "gemma", logger: quietLogger,\n    fetchImpl: async (_url, options = {}) => {\n      body = JSON.parse(options.body);\n      return ndjsonResponse([{ message: { content: "Use the safer reading." }, done: true, eval_count: 12 }], options.signal);\n    } });\n  const result = await runtime.respond({ ...turn, text: "Sensor A says safe, but sensor B says unsafe." });\n  assert.equal(result.kind, "response");\n  assert.equal(result.timings.reasoning_route, "THINK");\n  assert.equal(result.timings.think_enabled, true);\n  assert.equal(body.think, true);\n});\n\ntest("Gemma think override supports on/off/auto", async () => {\n  for (const [mode, expected] of [["on", true], ["off", false]]) {\n    let body;\n    const runtime = createAssistantRuntime({ enabled: true, profiles: gemmaProfiles({ [GEMMA_PROFILE_ID]: { think_mode: mode } }), preferredProfile: "gemma", logger: quietLogger,\n      fetchImpl: async (_url, options = {}) => { body = JSON.parse(options.body); return ndjsonResponse([{ message: { content: "ok" }, done: true }], options.signal); } });\n    await runtime.respond(turn);\n    assert.equal(body.think, expected);\n    assert.equal(runtime.getPreferredProfileConfig().think_mode, mode);\n  }\n});\n\ntest("Gemma availability failure falls back to Qwen", async () => {\n  const requests = [];\n  const runtime = createAssistantRuntime({ enabled: true, profiles: gemmaProfiles(), preferredProfile: "gemma", logger: quietLogger,\n    fetchImpl: async (url, options = {}) => {\n      requests.push(url);\n      if (url === "http://gemma.test/api/chat") throw new Error("offline");\n      return jsonResponse({ choices: [{ message: { content: "Qwen fallback." } }] });\n    } });\n  const result = await runtime.respond(turn);\n  assert.equal(result.text, "Qwen fallback.");\n  assert.deepEqual(requests, ["http://gemma.test/api/chat", "http://qwen.test/v1/chat/completions"]);\n  assert.equal(runtime.getTelemetry().effective_profile, QWEN_PROFILE_ID);\n});\n\ntest("Gemma health check recognizes the exact Hugging Face Ollama model", async () => {\n  const profiles = gemmaProfiles();\n  const runtime = createAssistantRuntime({ enabled: true, profiles, preferredProfile: "gemma", logger: quietLogger,\n    fetchImpl: async (url) => {\n      assert.equal(url, "http://gemma.test/api/tags");\n      return jsonResponse({ models: [{ name: profiles[GEMMA_PROFILE_ID].model }] });\n    } });\n  const telemetry = await runtime.refreshHealth();\n  assert.equal(telemetry.online, "online");\n});\n`;
  await writeFile(files.gemmaTest, test);
}

await patchProfiles();
await patchAssistant();
await patchTelegram();
await patchIndex();
await patchMenuTest();
await writeGemmaTests();

console.log("Gemma 4 profile migration applied.");
console.log("Run: node --check src/assistant-profiles.js && node --check src/assistant.js && node --check src/index.js");
console.log("Then: node --test test/gemma4-profile.test.js test/telegram-command-menu-contract.test.js");
