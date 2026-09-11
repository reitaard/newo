import assert from "node:assert/strict";
import test from "node:test";
import { createTrackLiveManager } from "../src/track-live.js";

function harness(edit = async () => {}) {
  const timers = [], edits = []; let state = { firmware: { desired: true, actual: "unknown" }, telemetry: { stale: true } };
  const manager = createTrackLiveManager({
    editMessage: async (...args) => { edits.push(args); return edit(...args); },
    render: ({ firmware, spinner }) => `${spinner}:${firmware.actual}`,
    getState: () => state,
    setTimer: (callback, delay) => { const timer = { callback, delay, cancelled: false }; timers.push(timer); return timer; },
    clearTimer: (timer) => { timer.cancelled = true; }, warmupMs: 1000, steadyMs: 5000,
  });
  const run = async () => { const timer = timers.find((item) => !item.cancelled && !item.ran); timer.ran = true; timer.callback(); await new Promise(setImmediate); };
  return { manager, timers, edits, run, setState: (value) => { state = value; } };
}

test("one panel per chat replaces the prior updater", () => {
  const h = harness(); h.manager.start(1, 10); const first = h.timers[0]; h.manager.start(1, 11);
  assert.equal(first.cancelled, true); assert.equal(h.manager.size(), 1);
});

test("warmup uses 1s and ACTIVE fresh telemetry uses a conservative edit budget", async () => {
  const h = harness(); h.manager.start(1, 10); assert.equal(h.timers[0].delay, 1000); await h.run();
  h.setState({ firmware: { desired: true, actual: "active" }, telemetry: { stale: false } });
  await h.run(); assert.equal(h.timers.at(-1).delay, 5000);
});

test("identical render is not edited twice", async () => {
  const h = harness(); h.manager.start(1, 10, "◐:unknown"); await h.run();
  assert.equal(h.edits.length, 0);
});

test("steady ACTIVE rendering does not rotate a spinner and force duplicate edits", async () => {
  const h = harness();
  h.setState({ firmware: { desired: true, actual: "active" }, telemetry: { stale: false } });
  h.manager.start(1, 10, "●:active");
  await h.run();
  assert.equal(h.edits.length, 0);
});

test("transient edit failure leaves tracking and updater running", async () => {
  let calls = 0; const h = harness(async () => { if (++calls === 1) throw new Error("temporary"); });
  h.manager.start(1, 10); await h.run(); assert.equal(h.manager.has(1), true); assert.equal(h.timers.at(-1).delay, 1000);
});

test("429 obeys retry_after", async () => {
  const error = Object.assign(new Error("rate"), { error_code: 429, parameters: { retry_after: 7 } });
  const h = harness(async () => { throw error; }); h.manager.start(1, 10); await h.run();
  assert.equal(h.timers.at(-1).delay, 7000);
});

test("desired OFF and permanent missing message terminate updater", async () => {
  const h = harness(); h.manager.start(1, 10); h.setState({ firmware: { desired: false, actual: "off", lastResult: "confirmed" }, telemetry: {} });
  await h.run(); assert.equal(h.manager.has(1), false);
  const error = Object.assign(new Error("gone"), { error_code: 400, description: "message to edit not found" });
  const p = harness(async () => { throw error; }); p.manager.start(2, 20); await p.run(); assert.equal(p.manager.has(2), false);
});
