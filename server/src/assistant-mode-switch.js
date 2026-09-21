import {
  ALFRED_SWITCH_READY,
  ALFRED_SWITCH_START,
  XIAOMEI_SWITCH_READY,
  XIAOMEI_SWITCH_START,
} from "./switch-earcon.js";

export const ASSISTANT_MODES = Object.freeze(["alfred", "xiaomei"]);

function normalizeMode(value) {
  const mode = String(value ?? "").trim().toLowerCase();
  return ASSISTANT_MODES.includes(mode) ? mode : null;
}

function requestError(label, response, detail) {
  const suffix = detail ? `: ${String(detail).slice(0, 240)}` : "";
  return new Error(`${label} failed (${response.status})${suffix}`);
}

export function createAssistantModeSwitch({
  enabled = false,
  initialMode = "alfred",
  ollamaBaseUrl,
  alfredModel,
  xiaomeiTtsBaseUrl,
  timeoutMs = 60_000,
  fetchImpl = fetch,
  playEarcon = async () => {},
  persistMode = async () => {},
  logger = null,
} = {}) {
  let mode = normalizeMode(initialMode) ?? "alfred";
  let switching = false;
  let target = null;
  let lastError = null;
  let queue = Promise.resolve();

  const trimUrl = (value) => String(value ?? "").replace(/\/+$/, "");
  const ollama = trimUrl(ollamaBaseUrl);
  const xiaomeiTts = trimUrl(xiaomeiTtsBaseUrl);

  async function postJson(url, body, label) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    timer.unref?.();
    let response;
    try {
      response = await fetchImpl(url, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: controller.signal,
      });
    } catch (error) {
      if (controller.signal.aborted) throw new Error(`${label} timed out`);
      throw new Error(`${label} unavailable: ${error?.message ?? "request failed"}`);
    } finally {
      clearTimeout(timer);
    }
    if (!response.ok) {
      const detail = await response.text().catch(() => "");
      throw requestError(label, response, detail);
    }
    return response;
  }

  async function setAlfredResident(resident) {
    if (!ollama || !alfredModel) throw new Error("Alfred Ollama endpoint is not configured");
    await postJson(`${ollama}/api/generate`, {
      model: alfredModel,
      prompt: "",
      stream: false,
      keep_alive: resident ? -1 : 0,
    }, resident ? "Alfred warmup" : "Alfred unload");
  }

  async function setXiaomeiResident(resident) {
    if (!xiaomeiTts) throw new Error("Xiaomei TTS endpoint is not configured");
    await postJson(`${xiaomeiTts}/admin/${resident ? "load" : "unload"}`, undefined,
      resident ? "Xiaomei load" : "Xiaomei unload");
  }

  async function earcon(text, phase) {
    try {
      await playEarcon(text);
    } catch (error) {
      logger?.warn?.({ event: "ASSISTANT_MODE_EARCON_FAILED", phase, error_message: error?.message ?? "unknown" },
        "Assistant mode earcon failed; continuing switch");
    }
  }

  async function commitMode(next) {
    await persistMode(next);
    mode = next;
  }

  async function enterXiaomei({ announce = true } = {}) {
    if (announce) await earcon(XIAOMEI_SWITCH_START, "xiaomei_start");
    await setAlfredResident(false);
    try {
      await setXiaomeiResident(true);
      await commitMode("xiaomei");
      if (announce) await earcon(XIAOMEI_SWITCH_READY, "xiaomei_ready");
    } catch (error) {
      await setXiaomeiResident(false).catch(() => {});
      await setAlfredResident(true).catch((rollbackError) => {
        logger?.error?.({ event: "ASSISTANT_MODE_ROLLBACK_FAILED", target: "alfred", error_message: rollbackError?.message ?? "unknown" },
          "Failed to restore Alfred after Xiaomei switch failure");
      });
      await commitMode("alfred").catch(() => { mode = "alfred"; });
      if (announce) await earcon(ALFRED_SWITCH_READY, "rollback_alfred_ready");
      throw error;
    }
  }

  async function enterAlfred({ announce = true } = {}) {
    if (announce) await earcon(ALFRED_SWITCH_START, "alfred_start");
    await setXiaomeiResident(false);
    try {
      await setAlfredResident(true);
      await commitMode("alfred");
      if (announce) await earcon(ALFRED_SWITCH_READY, "alfred_ready");
    } catch (error) {
      await setXiaomeiResident(true).catch((rollbackError) => {
        logger?.error?.({ event: "ASSISTANT_MODE_ROLLBACK_FAILED", target: "xiaomei", error_message: rollbackError?.message ?? "unknown" },
          "Failed to restore Xiaomei after Alfred switch failure");
      });
      await commitMode("xiaomei").catch(() => { mode = "xiaomei"; });
      if (announce) await earcon(XIAOMEI_SWITCH_READY, "rollback_xiaomei_ready");
      throw error;
    }
  }

  async function perform(next, { announce = true, force = false } = {}) {
    if (!enabled) throw new Error("Xiaomei mode switching is disabled");
    const normalized = normalizeMode(next);
    if (!normalized) throw new Error("invalid assistant mode");
    if (!force && normalized === mode) return status();
    switching = true;
    target = normalized;
    lastError = null;
    logger?.info?.({ event: "ASSISTANT_MODE_SWITCH_START", from: mode, to: normalized }, "Assistant mode switch started");
    try {
      if (normalized === "xiaomei") await enterXiaomei({ announce });
      else await enterAlfred({ announce });
      logger?.info?.({ event: "ASSISTANT_MODE_SWITCH_DONE", mode }, "Assistant mode switch complete");
      return status();
    } catch (error) {
      lastError = error?.message ?? "switch failed";
      logger?.error?.({ event: "ASSISTANT_MODE_SWITCH_FAILED", target: normalized, mode, error_message: lastError },
        "Assistant mode switch failed");
      throw error;
    } finally {
      switching = false;
      target = null;
    }
  }

  function enqueue(operation) {
    const result = queue.catch(() => {}).then(operation);
    queue = result.catch(() => {});
    return result;
  }

  function status() {
    return { enabled: Boolean(enabled), mode, switching, target, last_error: lastError,
      alfred_model: alfredModel ?? null, xiaomei_voice: "Serena" };
  }

  return {
    status,
    switchTo: (next, options) => enqueue(() => perform(next, options)),
    toggle: (options) => enqueue(() => perform(mode === "alfred" ? "xiaomei" : "alfred", options)),
    reconcile: () => enqueue(() => perform(mode, { announce: false, force: true })),
  };
}

function envBoolean(value, fallback = false) {
  if (value == null || String(value).trim() === "") return fallback;
  return String(value).trim().toLowerCase() === "true";
}

function envTimeout(value, fallback = 60_000) {
  const parsed = Number(value);
  return Number.isInteger(parsed) && parsed >= 1_000 && parsed <= 120_000 ? parsed : fallback;
}

function escapeHtml(value) {
  return String(value ?? "").replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;");
}

function telegramStatus(state) {
  const active = state.mode === "xiaomei" ? "Xiaomei" : "Alfred";
  const transition = state.switching ? `\nSwitching to: <b>${escapeHtml(state.target)}</b>` : "";
  return `<b><i>translate:</i></b>\n<blockquote>Active assistant: <b>${active}</b>\nXiaomei voice: <b>Serena</b>\nSwitch: <b>${state.switching ? "BUSY" : "READY"}</b>${transition}</blockquote>`;
}

export function createXiaomeiIntegration({ speakerRuntime, runtimeState, logger = null, env = process.env } = {}) {
  const enabled = envBoolean(env.XIAOMEI_ENABLED, false);
  const manager = createAssistantModeSwitch({
    enabled,
    initialMode: runtimeState?.assistantMode ?? "alfred",
    ollamaBaseUrl: env.XIAOMEI_OLLAMA_BASE_URL ?? "http://100.110.136.15:11435",
    alfredModel: env.XIAOMEI_ALFRED_MODEL ?? "newo-minicpm5:latest",
    xiaomeiTtsBaseUrl: env.XIAOMEI_TTS_BASE_URL ?? "http://100.110.136.15:8124",
    timeoutMs: envTimeout(env.XIAOMEI_SWITCH_TIMEOUT_MS),
    logger,
    persistMode: (mode) => runtimeState?.setAssistantMode?.(mode) ?? Promise.resolve(mode),
    playEarcon: async (text) => {
      const speech = speakerRuntime?.speak?.(text, { temporary: true, maxChars: 80,
        metadata: { assistant_mode_switch: true } });
      if (!speech || speech.kind === "disabled" || speech.kind === "offline") {
        throw new Error(`speaker ${speech?.kind ?? "unavailable"}`);
      }
      if (speech.kind !== "queued") throw new Error(`speaker ${speech.kind}`);
      await speech.completion;
    },
  });

  if (enabled) {
    const timer = setTimeout(() => {
      void manager.reconcile().catch((error) => logger?.error?.({ event: "ASSISTANT_MODE_RECONCILE_FAILED",
        error_message: error?.message ?? "unknown" }, "Assistant mode startup reconcile failed"));
    }, 0);
    timer.unref?.();
  }

  async function handleTelegram(ctx, commandReply) {
    const input = String(ctx.match ?? "").trim().toLowerCase();
    if (input === "status") return commandReply(ctx, telegramStatus(manager.status()), "response", null, { newoSpeak: false });
    if (!enabled) {
      return commandReply(ctx, `<b><i>translate:</i></b>\n<blockquote>Status: <b>DISABLED</b>\nSet XIAOMEI_ENABLED=true on the VPS to enable switching.</blockquote>`,
        "disabled", null, { newoSpeak: false });
    }
    if (input && !["on", "off"].includes(input)) {
      return commandReply(ctx, `<b><i>translate:</i></b>\n<blockquote>Use /translate to toggle, /translate on, /translate off, or /translate status.</blockquote>`,
        "usage", null, { newoSpeak: false });
    }
    try {
      const state = input === "on" ? await manager.switchTo("xiaomei")
        : input === "off" ? await manager.switchTo("alfred") : await manager.toggle();
      return commandReply(ctx, telegramStatus(state), "response", null, { newoSpeak: false });
    } catch (error) {
      return commandReply(ctx, `<b><i>translate:</i></b>\n<blockquote>Switch failed: <b>${escapeHtml(error?.message ?? "unknown")}</b>\nFallback: <b>${manager.status().mode === "alfred" ? "Alfred" : "Xiaomei"}</b></blockquote>`,
        "error", null, { newoSpeak: false });
    }
  }

  return { ...manager, handleTelegram };
}
