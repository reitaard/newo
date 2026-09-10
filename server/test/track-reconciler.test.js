import assert from "node:assert/strict";
import test from "node:test";
import { createTrackReconciler } from "../src/track-reconciler.js";

function harness(desired = true) {
  const timers = []; const requests = []; let handler = async () => ({ kind: "offline" });
  const reconciler = createTrackReconciler({ getDesired: () => desired,
    request: async (action) => { requests.push(action); return handler(action); },
    setTimer: (callback, delay) => { const item = { callback, delay, cancelled: false }; timers.push(item); return item; },
    clearTimer: (item) => { item.cancelled = true; }, backoffMs: [5, 10, 20] });
  const runNext = async () => { const item = timers.find((value) => !value.cancelled && !value.ran); item.ran = true; item.callback(); await new Promise(setImmediate); };
  return { reconciler, requests, timers, runNext, setDesired: (value) => { desired = value; },
           setHandler: (value) => { handler = value; } };
}

test("desired ON retries with bounded backoff until confirmed ACTIVE", async () => {
  const h = harness(); h.reconciler.start(); await h.runNext();
  assert.deepEqual(h.requests, ["on"]); assert.equal(h.timers.at(-1).delay, 5);
  await h.runNext(); assert.equal(h.timers.at(-1).delay, 10);
  h.setHandler(async () => ({ kind: "response", message: { state: "active", applied: true } }));
  await h.runNext(); assert.equal(h.reconciler.status().actual, "active");
  assert.equal(h.reconciler.status().nextRetryMs, null);
});

test("track OFF cancels pending ON retry immediately", async () => {
  const h = harness(); h.reconciler.start(); await h.runNext();
  h.setDesired(false); h.setHandler(async () => ({ kind: "response", message: { state: "off", applied: true } }));
  h.reconciler.desiredChanged(); await h.runNext();
  assert.deepEqual(h.requests, ["on", "off"]); assert.equal(h.reconciler.status().nextRetryMs, null);
});

test("new connection restarts a fresh recovery sequence", async () => {
  const h = harness(); h.reconciler.start(); await h.runNext(); h.reconciler.disconnected();
  h.setHandler(async () => ({ kind: "response", message: { state: "active", applied: true } }));
  h.reconciler.start(); await h.runNext();
  assert.deepEqual(h.requests, ["on", "on"]); assert.equal(h.reconciler.status().actual, "active");
});
