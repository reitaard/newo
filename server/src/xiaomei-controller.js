export const XIAOMEI_ROUTES = Object.freeze([
  "chat",
  "teach",
  "translate_once",
  "interpreter_start",
  "interpreter_translate",
  "interpreter_meta",
  "interpreter_stop",
]);

export const XIAOMEI_CONTROLLER_SCHEMA = Object.freeze({
  type: "object",
  properties: {
    route: { type: "string", enum: XIAOMEI_ROUTES },
    reply: { type: "string" },
  },
  required: ["route", "reply"],
  additionalProperties: false,
});

export const XIAOMEI_CONTROLLER_PROMPT = [
  "You are Xiaomei's bilingual English-Chinese intent controller.",
  `Choose exactly one route: ${XIAOMEI_ROUTES.join(", ")}.`,
  "chat = ordinary conversation.",
  "teach = learning, explanation, pronunciation, correction, grammar, or language practice.",
  "translate_once = one-off translation outside continuous interpreter mode, including requests to say the previous content in the other language.",
  "interpreter_start = start continuous translation between speakers.",
  "interpreter_translate = while interpreter mode is active, speech intended for the other person.",
  "interpreter_meta = while interpreter mode is active, a private question to Xiaomei about meaning, wording, tone, or the conversation.",
  "interpreter_stop = leave continuous interpreter mode.",
  "Return only the required JSON object.",
].join(" ");

function parseControllerOutput(text) {
  const trimmed = String(text ?? "").trim().replace(/^```(?:json)?\s*/i, "").replace(/\s*```$/i, "");
  const candidates = [trimmed];
  const first = trimmed.indexOf("{");
  const last = trimmed.lastIndexOf("}");
  if (first >= 0 && last > first) candidates.push(trimmed.slice(first, last + 1));
  for (const candidate of candidates) {
    try {
      const value = JSON.parse(candidate);
      if (value && typeof value === "object" && XIAOMEI_ROUTES.includes(value.route) && typeof value.reply === "string") {
        return value;
      }
    } catch {}
  }
  const error = new Error("invalid Xiaomei controller output");
  error.code = "xiaomei_controller_invalid";
  throw error;
}

function controllerContext(session) {
  return {
    mode: session.mode,
    interpreter: {
      active: session.interpreter.active,
      paused: session.interpreter.paused,
      sourceLanguage: session.interpreter.sourceLanguage,
      targetLanguage: session.interpreter.targetLanguage,
      recent: session.interpreter.recent.slice(-2),
    },
    previousTurn: session.lastTurn,
  };
}

export function createXiaomeiController({ client, logger = null }) {
  if (!client?.complete) throw new TypeError("Xiaomei controller requires a model client");

  async function classify({ text, session, signal }) {
    const user = JSON.stringify({ state: controllerContext(session), latest_user_speech: String(text ?? "") });
    const result = await client.complete({
      system: XIAOMEI_CONTROLLER_PROMPT,
      user,
      format: XIAOMEI_CONTROLLER_SCHEMA,
      temperature: 0.2,
      topP: 0.9,
      topK: 20,
      maxTokens: 192,
      contextSize: 4096,
      signal,
    });
    const decision = parseControllerOutput(result.text);
    logger?.info?.({ event: "XIAOMEI_CONTROLLER", route: decision.route, model: result.model,
      first_content_ms: result.firstContentMs, total_ms: result.latencyMs }, "Xiaomei semantic route resolved");
    return { ...decision, model: result.model, latencyMs: result.latencyMs, firstContentMs: result.firstContentMs };
  }

  return { classify };
}

export { parseControllerOutput };
