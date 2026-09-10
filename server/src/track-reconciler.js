const DEFAULT_BACKOFF_MS = Object.freeze([5_000, 15_000, 30_000, 60_000]);

export function createTrackReconciler({ getDesired, request, logger,
  setTimer = setTimeout, clearTimer = clearTimeout, backoffMs = DEFAULT_BACKOFF_MS }) {
  let connected = false, timer = null, generation = 0, attempt = 0;
  let actual = "unknown", lastResult = "idle", nextRetryMs = null;

  function cancel() {
    generation += 1; attempt = 0; nextRetryMs = null;
    if (timer !== null) clearTimer(timer);
    timer = null;
  }
  function schedule(delay, token) {
    nextRetryMs = delay;
    timer = setTimer(() => { timer = null; nextRetryMs = null; void run(token); }, delay);
    timer?.unref?.();
  }
  async function run(token) {
    if (token !== generation || !connected) return;
    const desired = Boolean(getDesired());
    const action = desired ? "on" : "off";
    let result;
    try { result = await request(action); }
    catch (error) { result = { kind: "error", error }; }
    if (token !== generation || !connected) return;
    const confirmed = result?.kind === "response" && result.message?.applied === true &&
      result.message.state === (desired ? "active" : "off");
    actual = result?.kind === "response" ? result.message?.state ?? "unknown" : "unknown";
    lastResult = confirmed ? "confirmed" : result?.kind ?? "error";
    logger?.[confirmed ? "info" : "warn"]?.({ desired_track: desired, actual_track: actual,
      reconcile_attempt: attempt + 1, result: lastResult },
      confirmed ? "Track desired state reconciled" : "Track desired state reconciliation pending");
    if (confirmed || !desired) { attempt = 0; return; }
    const delay = backoffMs[Math.min(attempt, backoffMs.length - 1)]; attempt += 1;
    schedule(delay, token);
  }
  function start() { cancel(); connected = true; const token = generation; schedule(0, token); }
  function disconnected() { connected = false; actual = "unknown"; cancel(); }
  function desiredChanged(reconcile = true) {
    cancel();
    if (connected && reconcile) { const token = generation; schedule(0, token); }
  }
  function commandResult(result) {
    cancel();
    const desired = Boolean(getDesired());
    const confirmed = result?.kind === "response" && result.message?.applied === true &&
      result.message.state === (desired ? "active" : "off");
    actual = result?.kind === "response" ? result.message?.state ?? "unknown" : "unknown";
    lastResult = confirmed ? "confirmed" : result?.kind ?? "offline";
    if (!confirmed && connected && desired) { const token = generation; schedule(backoffMs[0], token); }
  }
  return { start, disconnected, desiredChanged, commandResult,
    status: () => ({ desired: Boolean(getDesired()), actual, lastResult, attempt,
                     nextRetryMs, connected }) };
}
