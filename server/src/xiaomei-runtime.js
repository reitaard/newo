import { resolveXiaomeiCommand } from "./xiaomei-commands.js";
import { appendConversation, createXiaomeiSessionStore, recordInterpreterExchange, resetInterpreterState } from "./xiaomei-session.js";
import { interpreterMetaPrompt, teachingPrompt, XIAOMEI_PERSONA_PROMPT } from "./xiaomei-clients.js";

const hasHan = (text) => /\p{Script=Han}/u.test(String(text ?? ""));
const translationRequest = (text) => /^(?:translate\b|how (?:do|would) (?:i|you) say\b|what(?:'s| is) .+ in (?:chinese|english)\b)/i.test(text) ||
  /\b(?:say|put|translate) (?:that|it|this) in (?:chinese|english)\b/i.test(text);
const teachingRequest = (text) => /\b(?:teach|explain|pinyin|tone|grammar|pronounc|example|correct|quiz|practice|natural(?:ly)?)\b/i.test(text) ||
  /^how (?:do|would) (?:i|you) say\b/i.test(text) || /(?:练(?:一下)?中文|教我中文|学习中文)/u.test(text);
const interpreterStartHint = (text) => /\b(?:translate between us|interpreter mode|be my interpreter|translate .* until i (?:say|tell).*stop|with (?:a )?(?:chinese|english) speaker.*translate)\b/i.test(text);
const semanticHint = (text) => /\b(?:translate|interpreter|chinese|english|what did|what does|what .* mean|why did|explain|say that|say it|speaker)\b/i.test(text);
const interpreterMetaHint = (text) => /\b(?:what did|what does|what .* mean|why did|explain|don't translate|do not translate|was (?:she|he|that)|does (?:she|he|that) mean|tell me what|to me,? not (?:her|him|them))\b/i.test(text);

function explicitTarget(text) {
  if (/\b(?:into|in|to) chinese\b/i.test(text)) return "Chinese";
  if (/\b(?:into|in|to) english\b/i.test(text)) return "English";
  return null;
}

function directionFor(text, interpreter, target = null) {
  if (target) return { sourceLanguage: target === "Chinese" ? "English" : "Chinese", targetLanguage: target };
  if (interpreter.sourceLanguage !== "auto") return { sourceLanguage: interpreter.sourceLanguage, targetLanguage: interpreter.targetLanguage };
  return hasHan(text) ? { sourceLanguage: "Chinese", targetLanguage: "English" } :
    { sourceLanguage: "English", targetLanguage: "Chinese" };
}

function extractTranslationText(text, session) {
  const value = String(text ?? "");
  if (/\b(?:say|put|translate) (?:that|it|this) in (?:chinese|english)\b/i.test(value)) {
    return session.lastTurn?.assistantText || session.interpreter.lastTranslation || value;
  }
  const quoted = value.match(/["“'‘]([\s\S]+?)["”'’](?:\s+in\s+(?:chinese|english))?[?.!]*$/iu);
  if (quoted) return quoted[1];
  return value.replace(/^(?:translate(?: this once)?(?: into (?:chinese|english))?[: ]*|how (?:do|would) (?:i|you) say\s*)/i, "")
    .replace(/\s+(?:into|in) (?:chinese|english)[?.!]*$/i, "").trim();
}

export function routeXiaomeiTurn(text, session, command = resolveXiaomeiCommand(text, session)) {
  if (command) return { route: command.id === "interpreter.meta" ? "interpreter_meta" : "command", command };
  if (!session.active) return { route: "inactive" };
  if (session.mode === "interpreter" && session.interpreter.active) {
    if (session.interpreter.paused) return { route: "semantic", reason: "interpreter_paused" };
    if (interpreterMetaHint(text)) return { route: "semantic", reason: "possible_interpreter_meta" };
    return { route: "interpreter_translate" };
  }
  if (interpreterStartHint(text)) return { route: "semantic", reason: "interpreter_start_hint" };
  if (translationRequest(text)) return { route: "translate_once" };
  if (session.mode === "teach" || teachingRequest(text)) return { route: "teach" };
  if (semanticHint(text)) return { route: "semantic", reason: "language_intent_ambiguous" };
  return { route: "chat" };
}

function applyCommand(session, command) {
  switch (command.id) {
    case "session.activate":
      session.active = true; session.mode = "chat"; resetInterpreterState(session);
      return { text: "你好，我是玲玲。想聊天、学中文，还是让我帮你们翻译？" };
    case "session.exit":
      session.active = false; session.mode = "chat"; resetInterpreterState(session);
      return { text: "好的，下次见。" };
    case "mode.chat": session.mode = "chat"; resetInterpreterState(session); return { text: "好，我们聊聊天。" };
    case "mode.teach": session.mode = "teach"; resetInterpreterState(session); return { text: "好，我们来练中文。" };
    case "interpreter.start": session.mode = "interpreter"; session.interpreter.active = true; session.interpreter.paused = false; return { text: "翻译模式已开始。" };
    case "interpreter.stop": session.mode = "chat"; resetInterpreterState(session); return { text: "翻译模式已结束。" };
    case "interpreter.pause": session.interpreter.paused = true; return { text: "翻译已暂停。" };
    case "interpreter.resume": session.interpreter.paused = false; return { text: "继续翻译。" };
    case "interpreter.auto": session.interpreter.sourceLanguage = "auto"; session.interpreter.targetLanguage = "auto"; return { text: "我会自动判断语言。" };
    case "interpreter.en_zh": session.interpreter.sourceLanguage = "English"; session.interpreter.targetLanguage = "Chinese"; return { text: "现在从英语翻译成中文。" };
    case "interpreter.zh_en": session.interpreter.sourceLanguage = "Chinese"; session.interpreter.targetLanguage = "English"; return { text: "现在从中文翻译成英语。" };
    case "interpreter.switch": {
      const { sourceLanguage: source, targetLanguage: target } = session.interpreter;
      if (source === "auto") { session.interpreter.sourceLanguage = "English"; session.interpreter.targetLanguage = "Chinese"; }
      else { session.interpreter.sourceLanguage = target; session.interpreter.targetLanguage = source; }
      return { text: "语言方向已切换。" };
    }
    case "interpreter.preserve": session.interpreter.preserveTone = true; return { text: "我会保留语气和表达强度。" };
    case "interpreter.literal": session.interpreter.registerMode = "literal"; return { text: "好，我会尽量直译。" };
    case "interpreter.natural": session.interpreter.registerMode = "natural"; return { text: "好，我会用自然的表达来翻译。" };
    case "speech.repeat": return { text: session.teaching.lastPhrase ?? session.interpreter.lastTranslation ?? session.lastTurn.assistantText ?? "还没有可以重复的内容。" };
    case "speech.slower": return { text: session.teaching.lastPhrase ?? session.interpreter.lastTranslation ?? session.lastTurn.assistantText ?? "还没有可以慢速重复的内容。", speed: 0.75 };
    case "speech.normal": return { text: session.teaching.lastPhrase ?? session.lastTurn.assistantText ?? "好的。" };
    case "teach.chinese_only": return { text: session.teaching.lastPhrase ?? "还没有可以重复的中文。", language: "Chinese" };
    case "teach.more": session.mode = "teach"; session.teaching.depth = "detailed"; return { needsTeaching: true };
    case "teach.pinyin": session.mode = "teach"; return { needsTeaching: true, pronunciation: true };
    case "teach.practice": session.mode = "teach"; return { needsTeaching: true };
    default: return { needsTeaching: true };
  }
}

function updateRouteState(session, route) {
  if (route === "interpreter_start") {
    session.mode = "interpreter"; session.interpreter.active = true; session.interpreter.paused = false;
  } else if (route === "interpreter_stop") {
    session.mode = "chat"; resetInterpreterState(session);
  } else if (route === "teach") {
    session.mode = "teach";
  }
}

export function createXiaomeiRuntime({ gemma, translator, controller = null, speakerRuntime = null, serenaBackend = null,
  pronunciationProvider = null, logger = null, sessions = createXiaomeiSessionStore(), enabled = true,
  setAssistantState = () => {}, isActiveMode = () => true }) {
  const active = new Map();

  const translate = async (request) => {
    try { return await translator.translate(request); }
    catch (error) {
      if (request.signal?.aborted) throw error;
      const fallback = await gemma.complete({ system: [
        "Translate only. Output only the natural translation.",
        `Source: ${request.sourceLanguage}. Target: ${request.targetLanguage}. Preserve meaning, numbers, negation, tone, and register.`,
      ].join(" "), user: request.text, temperature: 0, maxTokens: 256, signal: request.signal });
      logger?.warn?.({ event: "XIAOMEI_TRANSLATION_FALLBACK", error: error?.code ?? error?.message }, "Hy-MT2 unavailable; Gemma translated");
      return fallback;
    }
  };

  const cancelDevice = (deviceId, reason = "xiaomei_cancelled") => {
    const current = active.get(deviceId);
    if (!current) return false;
    const error = new Error(reason); error.code = reason;
    current.controller.abort(error);
    current.speech?.cancel?.();
    active.delete(deviceId);
    return true;
  };

  async function speak(deviceId, session, text, { generationId, turn, speed = 1, segments = null, language = null } = {}) {
    if (!speakerRuntime || !text) return null;
    const speech = speakerRuntime.speak(text, {
      backend: serenaBackend,
      segments,
      synthesisOptions: { speed, ...(language ? { language } : {}) },
      metadata: { assistant_turn: true, xiaomei: true, generation_id: generationId,
        voice_stream_id: turn?.streamId ?? null, route: session.lastTurn.route },
    });
    if (speech?.kind === "queued" && active.get(deviceId)) active.get(deviceId).speech = speech;
    return speech;
  }

  async function resolveSemantic(text, session, signal) {
    if (!controller?.classify) return { route: session.mode === "interpreter" ? "interpreter_meta" : "chat", reply: "" };
    try { return await controller.classify({ text, session, signal }); }
    catch (error) {
      logger?.warn?.({ event: "XIAOMEI_CONTROLLER_FALLBACK", error: error?.code ?? error?.message }, "Xiaomei semantic controller failed");
      return { route: session.mode === "interpreter" ? "interpreter_meta" : "chat", reply: "" };
    }
  }

  async function handleTranscript(turn, { dryRun = false } = {}) {
    if (!enabled || !isActiveMode()) return false;
    const started = performance.now();
    const session = sessions.get(turn.deviceId);
    if (!session.active) session.active = true;

    cancelDevice(turn.deviceId, "xiaomei_superseded");
    const abortController = new AbortController();
    const generationId = ++session.generation;
    active.set(turn.deviceId, { controller: abortController, generationId, speech: null });
    setAssistantState(turn.deviceId, "thinking");

    let command = null;
    let decision = null;
    let controllerTiming = null;
    let text = null;
    let model = null;
    let modelMs = null;
    let firstContentMs = null;
    let direction = null;
    let segments = null;
    let speed = 1;
    let language = null;

    try {
      const commandStarted = performance.now();
      command = resolveXiaomeiCommand(turn.text, session);
      const commandMs = performance.now() - commandStarted;
      const routeStarted = performance.now();
      decision = routeXiaomeiTurn(turn.text, session, command);
      let routeMs = performance.now() - routeStarted;

      if (decision.route === "semantic") {
        const semanticStarted = performance.now();
        const semantic = await resolveSemantic(turn.text, session, abortController.signal);
        controllerTiming = { totalMs: semantic.latencyMs ?? Math.round(performance.now() - semanticStarted), firstContentMs: semantic.firstContentMs ?? null, model: semantic.model ?? null };
        decision = { route: semantic.route, semanticReply: semantic.reply ?? "" };
        routeMs += performance.now() - semanticStarted;
        updateRouteState(session, decision.route);
      }

      if (decision.route === "command") {
        const action = applyCommand(session, decision.command);
        text = action.text ?? null;
        speed = action.speed ?? speed;
        language = action.language ?? language;
        if (action.needsTeaching) {
          const verified = action.pronunciation ? await pronunciationProvider?.lookup?.(session.teaching.lastPhrase ?? session.teaching.pronunciationTarget ?? turn.text) : null;
          const context = { lastPhrase: session.teaching.lastPhrase, lastExplanation: session.teaching.lastExplanation,
            currentTopic: session.teaching.currentTopic };
          const result = await gemma.complete({ system: teachingPrompt(verified, context), user: turn.text, signal: abortController.signal });
          text = result.text; model = result.model; modelMs = result.latencyMs; firstContentMs = result.firstContentMs;
        }
      } else if (decision.route === "interpreter_start") {
        session.mode = "interpreter"; session.interpreter.active = true; session.interpreter.paused = false;
        text = decision.semanticReply || "翻译模式已开始。";
      } else if (decision.route === "interpreter_stop") {
        session.mode = "chat"; resetInterpreterState(session);
        text = decision.semanticReply || "翻译模式已结束。";
      } else if (decision.route === "interpreter_meta") {
        const result = await gemma.complete({ system: interpreterMetaPrompt(session), user: turn.text, signal: abortController.signal });
        text = result.text; model = result.model; modelMs = result.latencyMs; firstContentMs = result.firstContentMs;
      } else if (decision.route === "interpreter_translate") {
        direction = directionFor(turn.text, session.interpreter);
        const result = await translate({ text: turn.text, ...direction, preserveTone: session.interpreter.preserveTone,
          registerMode: session.interpreter.registerMode ?? "natural", signal: abortController.signal });
        text = result.text; model = result.model; modelMs = result.latencyMs; firstContentMs = result.firstContentMs;
        recordInterpreterExchange(session, turn.text, text, direction);
      } else if (decision.route === "translate_once") {
        const source = extractTranslationText(turn.text, session);
        const target = explicitTarget(turn.text);
        direction = directionFor(source, session.interpreter, target);
        const translated = await translate({ text: source, ...direction, preserveTone: true, signal: abortController.signal });
        text = translated.text; model = translated.model; modelMs = translated.latencyMs; firstContentMs = translated.firstContentMs;
        session.teaching.lastPhrase = translated.text;
        if (teachingRequest(turn.text)) {
          const explained = await gemma.complete({ system: teachingPrompt(null, { translation: translated.text }),
            user: `Translation: ${translated.text}\nExplain the most useful nuance briefly.`, signal: abortController.signal });
          text = `${translated.text} ${explained.text}`;
          model = `${translated.model}+${explained.model}`;
          modelMs = (modelMs ?? 0) + (explained.latencyMs ?? 0);
          segments = [{ text: translated.text, language: direction.targetLanguage }, { text: explained.text, language: "Auto" }];
        }
      } else if (decision.route === "teach") {
        session.mode = "teach";
        const verified = await pronunciationProvider?.lookup?.(turn.text);
        const context = { lastPhrase: session.teaching.lastPhrase, lastExplanation: session.teaching.lastExplanation,
          currentTopic: session.teaching.currentTopic };
        const result = await gemma.complete({ system: teachingPrompt(verified, context),
          messages: [...session.conversation.slice(-6), { role: "user", content: turn.text }], signal: abortController.signal });
        text = result.text; model = result.model; modelMs = result.latencyMs; firstContentMs = result.firstContentMs;
      } else {
        const result = await gemma.complete({ system: XIAOMEI_PERSONA_PROMPT,
          messages: [...session.conversation.slice(-6), { role: "user", content: turn.text }], signal: abortController.signal });
        text = result.text; model = result.model; modelMs = result.latencyMs; firstContentMs = result.firstContentMs;
      }

      if (session.generation !== generationId) return { kind: "cancelled", generationId };
      session.lastTurn = { userText: turn.text, assistantText: text, route: decision.route, model };
      appendConversation(session, "user", turn.text);
      appendConversation(session, "assistant", text);
      if (session.mode === "teach" && text) session.teaching.lastExplanation = text;

      setAssistantState(turn.deviceId, "responding");
      const speech = dryRun ? null : await speak(turn.deviceId, session, text, { generationId, turn, speed, segments, language });
      if (speech?.kind === "queued") await speech.completion;
      const speechMetrics = speech?.metrics ?? speech?.source?.metrics ?? null;
      const timing = {
        commandMs: Math.round(commandMs),
        routeMs: Math.round(routeMs),
        controllerMs: controllerTiming?.totalMs ?? null,
        controllerFirstContentMs: controllerTiming?.firstContentMs ?? null,
        modelMs,
        modelFirstContentMs: firstContentMs ?? null,
        ttsFirstAudioMs: speechMetrics?.firstAudioByteAt && speechMetrics?.requestStartedAt ? Math.round(speechMetrics.firstAudioByteAt - speechMetrics.requestStartedAt) : null,
        totalMs: Math.round(performance.now() - started),
      };
      logger?.info?.({ event: "XIAOMEI_TURN", device_id: turn.deviceId, stream_id: turn.streamId,
        generation_id: generationId, route: decision.route, model, direction, ...timing }, "Xiaomei turn completed");
      return { kind: "response", text, route: decision.route, model, direction, timing, speech, session: structuredClone(session) };
    } catch (error) {
      if (abortController.signal.aborted) return { kind: "cancelled", error: abortController.signal.reason?.code ?? "xiaomei_cancelled" };
      logger?.warn?.({ event: "XIAOMEI_ERROR", route: decision?.route ?? null, error: error?.code ?? error?.message }, "Xiaomei turn failed");
      return { kind: "error", error: error?.code ?? "xiaomei_failed", route: decision?.route ?? null };
    } finally {
      if (active.get(turn.deviceId)?.generationId === generationId) active.delete(turn.deviceId);
      if (session.generation === generationId) setAssistantState(turn.deviceId, "idle");
    }
  }

  return {
    handleTranscript,
    interruptDevice: cancelDevice,
    getSession: (deviceId) => sessions.snapshot(deviceId),
    resetSession: (deviceId) => sessions.reset(deviceId),
  };
}
