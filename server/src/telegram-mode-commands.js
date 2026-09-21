import { createPrimaryModeHandlers as createCoreModeHandlers } from "./telegram-mode-commands-core.js";
import { createAssistantModeSwitch } from "./assistant-mode-switch.js";
import { getActiveRuntimeStateStore } from "./runtime-state.js";
import {
  ALFRED_SWITCH_READY,
  ALFRED_SWITCH_START,
  XIAOMEI_SWITCH_READY,
  XIAOMEI_SWITCH_START,
} from "./switch-earcon.js";

export * from "./telegram-mode-commands-core.js";

const SWITCH_MESSAGES = new Map([
  [XIAOMEI_SWITCH_START, "Switching to Xiaomei."],
  [XIAOMEI_SWITCH_READY, "Xiaomei ready."],
  [ALFRED_SWITCH_START, "Switching to Alfred."],
  [ALFRED_SWITCH_READY, "Alfred ready."],
]);
const EARCON_SETTLE_MS = 450;

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

function modeStatus(state) {
  const active = state.mode === "xiaomei" ? "Xiaomei" : "Alfred";
  const switching = state.switching ? `\nSwitching to: <b>${escapeHtml(state.target)}</b>` : "";
  return `<b><i>translate:</i></b>\n<blockquote>Active assistant: <b>${active}</b>\nXiaomei voice: <b>Serena</b>\nSwitch: <b>${state.switching ? "BUSY" : "READY"}</b>${switching}</blockquote>`;
}

async function installTranslateCommandInTelegramMenu() {
  const token = process.env.TELEGRAM_BOT_TOKEN;
  if (!token) return;
  const base = `https://api.telegram.org/bot${token}`;
  try {
    const currentResponse = await fetch(`${base}/getMyCommands`);
    if (!currentResponse.ok) return;
    const current = await currentResponse.json();
    if (!current?.ok || !Array.isArray(current.result)) return;
    const retained = current.result.filter((item) => !["xiaomei", "xm", "translate"].includes(item?.command));
    const commands = [...retained, { command: "translate", description: "Toggle Alfred / Xiaomei translator" }];
    await fetch(`${base}/setMyCommands`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ commands }),
    });
  } catch {
    // The slash command still works through the final text middleware even if
    // Telegram's optional command menu update is temporarily unavailable.
  }
}

export function createPrimaryModeHandlers(options) {
  const core = createCoreModeHandlers(options);
  const runtimeState = getActiveRuntimeStateStore();
  const enabled = envBoolean(process.env.XIAOMEI_ENABLED, false);
  let activeSwitchContext = null;

  const modeSwitch = createAssistantModeSwitch({
    enabled,
    initialMode: runtimeState?.assistantMode ?? "alfred",
    ollamaBaseUrl: process.env.XIAOMEI_OLLAMA_BASE_URL ?? "http://100.110.136.15:11435",
    alfredModel: process.env.XIAOMEI_ALFRED_MODEL ?? "newo-minicpm5:latest",
    xiaomeiTtsBaseUrl: process.env.XIAOMEI_TTS_BASE_URL ?? "http://100.110.136.15:8124",
    timeoutMs: envTimeout(process.env.XIAOMEI_SWITCH_TIMEOUT_MS),
    persistMode: (mode) => runtimeState?.setAssistantMode?.(mode) ?? Promise.resolve(mode),
    playEarcon: async (earcon) => {
      const ctx = activeSwitchContext;
      if (!ctx) return;
      const text = SWITCH_MESSAGES.get(earcon);
      if (!text) return;
      const trace = options.commandTrace?.(ctx);
      if (trace) trace.speechQueued = false;
      await options.commandReply(ctx, text, "assistant_mode_switch", null, { newoSpeak: true, newoSpeakMaxChars: 80 });
      await new Promise((resolve) => setTimeout(resolve, EARCON_SETTLE_MS));
    },
  });

  if (enabled) {
    const reconcileTimer = setTimeout(() => { void modeSwitch.reconcile().catch(() => {}); }, 0);
    reconcileTimer.unref?.();
    const menuTimer = setTimeout(() => { void installTranslateCommandInTelegramMenu(); }, 3_000);
    menuTimer.unref?.();
  }

  async function setTemporarySpeaker(enabledForSwitch) {
    options.setSpeakerAccepting(enabledForSwitch);
    const request = options.sendDeviceRequest("speaker_control", "speaker_ack",
      { action: "set_enabled", enabled: enabledForSwitch, led_feedback: false });
    if (request.kind === "sent") await request.promise.catch?.(() => {});
  }

  async function runSwitchWithAudibleCue(ctx, operation) {
    const speakerWasEnabled = options.getSpeakerEnabled();
    if (!speakerWasEnabled) await setTemporarySpeaker(true);
    activeSwitchContext = ctx;
    try {
      return await operation();
    } finally {
      activeSwitchContext = null;
      if (!speakerWasEnabled) await setTemporarySpeaker(false);
    }
  }

  async function translate(ctx, forced = null) {
    const input = String(forced ?? ctx.match ?? "").trim().toLowerCase();
    if (input === "status") {
      return options.commandReply(ctx, modeStatus(modeSwitch.status()), "response", null, { newoSpeak: false });
    }
    if (!enabled) {
      return options.commandReply(ctx,
        `<b><i>translate:</i></b>\n<blockquote>Status: <b>DISABLED</b>\nSet XIAOMEI_ENABLED=true on the VPS to enable switching.</blockquote>`,
        "disabled", null, { newoSpeak: false });
    }
    if (input && input !== "on" && input !== "off") {
      return options.commandReply(ctx,
        `<b><i>translate:</i></b>\n<blockquote>Use /translate to toggle, /translate on, /translate off, or /translate status.</blockquote>`,
        "usage", null, { newoSpeak: false });
    }
    try {
      await runSwitchWithAudibleCue(ctx, () => input === "on" ? modeSwitch.switchTo("xiaomei")
        : input === "off" ? modeSwitch.switchTo("alfred") : modeSwitch.toggle());
      return null;
    } catch (error) {
      const fallback = modeSwitch.status().mode === "alfred" ? "Alfred" : "Xiaomei";
      return options.commandReply(ctx,
        `<b><i>translate:</i></b>\n<blockquote>Switch failed: <b>${escapeHtml(error?.message ?? "unknown")}</b>\nFallback: <b>${fallback}</b></blockquote>`,
        "error", null, { newoSpeak: false });
    }
  }

  async function profilePromptInput(ctx) {
    const raw = String(ctx.message?.text ?? "").trim();
    const command = raw.match(/^\/translate(?:@[a-z0-9_]+)?(?:\s+(.+))?$/i);
    if (command) {
      await translate(ctx, command[1] ?? "");
      return true;
    }
    return core.profilePromptInput(ctx);
  }

  return { ...core, translate, profilePromptInput, xiaomeiStatus: modeSwitch.status };
}
