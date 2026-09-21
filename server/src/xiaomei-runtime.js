import { resolveXiaomeiCommand } from "./xiaomei-commands.js";
import { createXiaomeiSessionStore, recordInterpreterExchange } from "./xiaomei-session.js";
import { interpreterMetaPrompt, teachingPrompt, XIAOMEI_PERSONA_PROMPT } from "./xiaomei-clients.js";

const hasHan = (text) => /\p{Script=Han}/u.test(text);
const translationRequest = (text) => /^(?:translate|how (?:do|would) (?:i|you) say|what(?:'s| is) .+ in (?:chinese|english))\b/i.test(text);
const teachingRequest = (text) => /\b(?:teach|explain|pinyin|tone|grammar|pronounc|example|correct|quiz|practice|natural(?:ly)?)\b/i.test(text) ||
  /^how (?:do|would) (?:i|you) say\b/i.test(text) || /(?:练(?:一下)?中文|教我中文|学习中文)/u.test(text);

function directionFor(text, interpreter) {
  if (interpreter.sourceLanguage !== "auto") return { sourceLanguage: interpreter.sourceLanguage, targetLanguage: interpreter.targetLanguage };
  return hasHan(text) ? { sourceLanguage: "Chinese", targetLanguage: "English" } :
    { sourceLanguage: "English", targetLanguage: "Chinese" };
}

function extractTranslationText(text) {
  const quoted = String(text).match(/["“'‘]([\s\S]+?)["”'’](?:\s+in\s+(?:chinese|english))?[?.!]*$/iu);
  if (quoted) return quoted[1];
  return String(text).replace(/^(?:translate|how (?:do|would) (?:i|you) say)\s*/i, "").replace(/\s+in (?:chinese|english)[?.!]*$/i, "").trim();
}

export function routeXiaomeiTurn(text, session, command = resolveXiaomeiCommand(text, session)) {
  if (command) return { route: command.id === "interpreter.meta" ? "interpreter_meta" : "command", command };
  if (!session.active) return { route: "inactive" };
  if (session.mode === "interpreter" && session.interpreter.active && !session.interpreter.paused)
    return { route: "interpreter_translate" };
  if (translationRequest(text)) return { route: "translate_once" };
  if (session.mode === "teach" || teachingRequest(text)) return { route: "teach" };
  return { route: "chat" };
}

function applyCommand(session, command) {
  switch (command.id) {
    case "session.activate": session.active = true; session.mode = "chat"; return "你好，我是玲玲。想聊天、学中文，还是让我帮你们翻译？";
    case "session.exit": session.active = false; session.mode = "chat"; session.interpreter.active = false; return "好的，下次见。";
    case "mode.chat": session.mode = "chat"; session.interpreter.active = false; return "好，我们聊聊天。";
    case "mode.teach": session.mode = "teach"; session.interpreter.active = false; return "好，我们来练中文。";
    case "interpreter.start": session.mode = "interpreter"; session.interpreter.active = true; session.interpreter.paused = false; return "翻译模式已开始。";
    case "interpreter.stop": session.mode = "chat"; session.interpreter.active = false; session.interpreter.paused = false; return "翻译模式已结束。";
    case "interpreter.pause": session.interpreter.paused = true; return "翻译已暂停。";
    case "interpreter.resume": session.interpreter.paused = false; return "继续翻译。";
    case "interpreter.auto": session.interpreter.sourceLanguage = "auto"; session.interpreter.targetLanguage = "auto"; return "我会自动判断语言。";
    case "interpreter.en_zh": session.interpreter.sourceLanguage = "English"; session.interpreter.targetLanguage = "Chinese"; return "现在从英语翻译成中文。";
    case "interpreter.zh_en": session.interpreter.sourceLanguage = "Chinese"; session.interpreter.targetLanguage = "English"; return "现在从中文翻译成英语。";
    case "interpreter.switch": {
      const source = session.interpreter.sourceLanguage, target = session.interpreter.targetLanguage;
      if (source === "auto") { session.interpreter.sourceLanguage = "English"; session.interpreter.targetLanguage = "Chinese"; }
      else { session.interpreter.sourceLanguage = target; session.interpreter.targetLanguage = source; }
      return "语言方向已切换。";
    }
    case "interpreter.preserve": session.interpreter.preserveTone = true; return "我会保留语气和表达强度。";
    case "speech.repeat": return session.teaching.lastPhrase ?? session.lastTurn.assistantText ?? "还没有可以重复的内容。";
    case "speech.slower": return session.teaching.lastPhrase ?? session.interpreter.lastTranslation ?? session.lastTurn.assistantText ?? "还没有可以慢速重复的内容。";
    case "speech.normal": return session.teaching.lastPhrase ?? session.lastTurn.assistantText ?? "好的。";
    case "teach.more": session.mode = "teach"; session.teaching.depth = "detailed"; return null;
    case "teach.pinyin": session.mode = "teach"; return null;
    case "teach.practice": session.mode = "teach"; return null;
    default: return null;
  }
}

export function createXiaomeiRuntime({ gemma, translator, speakerRuntime = null, serenaBackend = null,
  pronunciationProvider = null, logger = null, sessions = createXiaomeiSessionStore(), enabled = true,
  setAssistantState = () => {} }) {
  const active = new Map();
  const translate = async (request) => {
    try { return await translator.translate(request); }
    catch (error) {
      if (request.signal?.aborted) throw error;
      const fallback = await gemma.complete({ system: ["Translate only. Return only the natural translation.",
        `Source: ${request.sourceLanguage}. Target: ${request.targetLanguage}. Preserve meaning, numbers, negation, tone, and register.`].join(" "),
        user: request.text, temperature: 0, maxTokens: 256, signal: request.signal });
      logger?.warn({ event: "XIAOMEI_TRANSLATION_FALLBACK", error: error?.code ?? error?.message }, "Hy-MT2 unavailable; Gemma translated");
      return fallback;
    }
  };
  const cancelDevice = (deviceId, reason = "xiaomei_cancelled") => {
    const current = active.get(deviceId); if (!current) return false;
    const error = new Error(reason); error.code = reason; current.controller.abort(error); current.speech?.cancel?.(); active.delete(deviceId); return true;
  };

  async function speak(deviceId, session, text, { generationId, turn, speed = 1, segments = null } = {}) {
    if (!speakerRuntime || !text) return null;
    const speech = speakerRuntime.speak(text, { backend: serenaBackend, segments, synthesisOptions: { speed }, metadata: { assistant_turn: true,
      xiaomei: true, generation_id: generationId, voice_stream_id: turn?.streamId ?? null, route: session.lastTurn.route } });
    if (speech?.kind === "queued") active.get(deviceId).speech = speech;
    return speech;
  }

  async function handleTranscript(turn, { dryRun = false } = {}) {
    if (!enabled) return false;
    const session = sessions.get(turn.deviceId);
    const command = resolveXiaomeiCommand(turn.text, session);
    if (!session.active && command?.id !== "session.activate") return false;
    cancelDevice(turn.deviceId, "xiaomei_superseded");
    const controller = new AbortController();
    const generationId = ++session.generation;
    active.set(turn.deviceId, { controller, generationId, speech: null });
    setAssistantState(turn.deviceId, "thinking");
    const started = performance.now(), routedAt = performance.now();
    const decision = routeXiaomeiTurn(turn.text, session, command);
    let text = null, model = null, modelMs = null, direction = null, segments = null, speed = 1;
    try {
      if (decision.route === "command") {
        text = applyCommand(session, decision.command);
        if (decision.command.id === "speech.slower") speed = 0.75;
        if (text == null) {
          const verified = await pronunciationProvider?.lookup?.(session.teaching.pronunciationTarget ?? turn.text);
          const result = await gemma.complete({ system: teachingPrompt(verified), user: turn.text, signal: controller.signal });
          text = result.text; model = result.model; modelMs = result.latencyMs;
        }
      } else if (decision.route === "interpreter_meta") {
        const result = await gemma.complete({ system: interpreterMetaPrompt(session), user: turn.text, signal: controller.signal });
        text = result.text; model = result.model; modelMs = result.latencyMs;
      } else if (decision.route === "interpreter_translate") {
        direction = directionFor(turn.text, session.interpreter);
        const result = await translate({ text: turn.text, ...direction,
          preserveTone: session.interpreter.preserveTone, signal: controller.signal });
        text = result.text; model = result.model; modelMs = result.latencyMs;
        recordInterpreterExchange(session, turn.text, text, direction);
      } else if (decision.route === "translate_once") {
        const source = extractTranslationText(turn.text);
        direction = directionFor(source, session.interpreter);
        const translated = await translate({ text: source, ...direction, preserveTone: true, signal: controller.signal });
        text = translated.text; model = translated.model; modelMs = translated.latencyMs;
        session.teaching.lastPhrase = text;
        if (teachingRequest(turn.text)) {
          const explained = await gemma.complete({ system: teachingPrompt(),
            user: `Translation: ${text}\nExplain the most useful nuance briefly.`, signal: controller.signal });
          text = `${text} ${explained.text}`; model = `${translated.model}+${explained.model}`; modelMs += explained.latencyMs;
          segments = [{ text: translated.text, language: direction.targetLanguage }, { text: explained.text, language: "Auto" }];
        }
      } else if (decision.route === "teach") {
        session.mode = "teach";
        const verified = await pronunciationProvider?.lookup?.(turn.text);
        const result = await gemma.complete({ system: teachingPrompt(verified), user: turn.text, signal: controller.signal });
        text = result.text; model = result.model; modelMs = result.latencyMs;
      } else {
        const result = await gemma.complete({ system: XIAOMEI_PERSONA_PROMPT, user: turn.text, signal: controller.signal });
        text = result.text; model = result.model; modelMs = result.latencyMs;
      }
      if (session.generation !== generationId) return { kind: "cancelled", generationId };
      session.lastTurn = { userText: turn.text, assistantText: text, route: decision.route, model };
      if (session.mode === "teach" && text) session.teaching.lastExplanation = text;
      setAssistantState(turn.deviceId, "responding");
      const speech = dryRun ? null : await speak(turn.deviceId, session, text, { generationId, turn, speed, segments });
      if (speech?.kind === "queued") await speech.completion;
      const timing = { routeMs: Math.round(routedAt - started), modelMs, totalMs: Math.round(performance.now() - started) };
      logger?.info({ event: "XIAOMEI_TURN", device_id: turn.deviceId, stream_id: turn.streamId, generation_id: generationId,
        route: decision.route, model, direction, ...timing }, "Xiaomei turn completed");
      return { kind: "response", text, route: decision.route, model, direction, timing, speech, session: structuredClone(session) };
    } catch (error) {
      if (controller.signal.aborted) return { kind: "cancelled", error: controller.signal.reason?.code ?? "xiaomei_cancelled" };
      logger?.warn({ event: "XIAOMEI_ERROR", route: decision.route, error: error?.code ?? error?.message }, "Xiaomei turn failed");
      return { kind: "error", error: error?.code ?? "xiaomei_failed", route: decision.route };
    } finally {
      if (active.get(turn.deviceId)?.generationId === generationId) active.delete(turn.deviceId);
      if (session.generation === generationId) setAssistantState(turn.deviceId, "idle");
    }
  }

  return { handleTranscript, interruptDevice: cancelDevice, getSession: (deviceId) => sessions.snapshot(deviceId), resetSession: (deviceId) => sessions.reset(deviceId) };
}
