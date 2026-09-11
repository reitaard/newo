const SPINNERS = ["◐", "◓", "◑", "◒"];

function retryAfterMs(error) {
  const seconds = error?.parameters?.retry_after ?? error?.error?.parameters?.retry_after;
  return Number.isFinite(seconds) && seconds > 0 ? seconds * 1_000 : null;
}

function permanentEditFailure(error) {
  const code = error?.error_code ?? error?.error?.error_code;
  const description = String(error?.description ?? error?.error?.description ?? error?.message ?? "").toLowerCase();
  return code === 400 && (description.includes("message to edit not found") || description.includes("message can't be edited"));
}

export function createTrackLiveManager({ editMessage, render, getState, logger,
  setTimer = setTimeout, clearTimer = clearTimeout, warmupMs = 1_000, steadyMs = 2_000 }) {
  const panels = new Map();

  function cancel(panel) { if (panel.timer !== null) clearTimer(panel.timer); panel.timer = null; panel.stopped = true; }
  function schedule(panel, delay) {
    if (panel.stopped) return;
    panel.timer = setTimer(() => { panel.timer = null; void tick(panel); }, delay);
    panel.timer?.unref?.();
  }
  async function tick(panel, final = false) {
    if (panel.stopped && !final) return;
    const state = getState();
    const stateStopsPanel = state.firmware?.desired === false ||
      (state.firmware?.actual === "off" && state.firmware?.lastResult === "confirmed");
    const spinner = SPINNERS[panel.frame++ % SPINNERS.length];
    const rendered = render({ ...state, spinner, now: new Date() });
    if (rendered !== panel.lastText) {
      try {
        await editMessage(panel.chatId, panel.messageId, rendered, { parse_mode: "HTML" });
        panel.lastText = rendered;
      } catch (error) {
        if (permanentEditFailure(error)) { cancel(panel); panels.delete(panel.chatId); return; }
        const retry = retryAfterMs(error);
        logger?.warn?.({ chat_id: panel.chatId, retry_after_ms: retry, error_message: error?.message }, "Track panel edit failed");
        if (!final) { schedule(panel, retry ?? warmupMs); return; }
      }
    }
    if (final || panel.stopped || stateStopsPanel) {
      if (stateStopsPanel) { cancel(panel); panels.delete(panel.chatId); }
      return;
    }
    const next = state.firmware?.actual === "active" && !state.telemetry?.stale ? steadyMs : warmupMs;
    schedule(panel, next);
  }
  function start(chatId, messageId, initialText = null) {
    const previous = panels.get(chatId);
    if (previous) cancel(previous);
    const panel = { chatId, messageId, lastText: initialText, frame: 0, timer: null, stopped: false };
    panels.set(chatId, panel); schedule(panel, warmupMs); return panel;
  }
  async function stop(chatId, { final = true } = {}) {
    const panel = panels.get(chatId); if (!panel) return false;
    cancel(panel); panels.delete(chatId); if (final) await tick(panel, true); return true;
  }
  function stopAll() { for (const panel of panels.values()) cancel(panel); panels.clear(); }
  return { start, stop, stopAll, has: (chatId) => panels.has(chatId), size: () => panels.size };
}
