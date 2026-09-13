import assert from "node:assert/strict";
import test from "node:test";

import { createTelegramUpdateAcceptor } from "../src/telegram-webhook.js";

test("duplicate update_id is accepted without repeating side effects", async () => {
  const seen = new Set(), scheduled = [], handled = [];
  const acceptor = createTelegramUpdateAcceptor({
    acceptUpdateId: async (id) => seen.has(id) ? false : (seen.add(id), true),
    handleUpdate: async (update) => handled.push(update.update_id),
    schedule: (callback) => scheduled.push(callback),
  });
  assert.deepEqual(await acceptor.accept({ update_id: 41 }), { accepted: true, duplicate: false });
  assert.deepEqual(await acceptor.accept({ update_id: 41 }), { accepted: true, duplicate: true });
  assert.equal(scheduled.length, 1);
  scheduled[0](); await new Promise(setImmediate);
  assert.deepEqual(handled, [41]);
});

test("outbound Telegram failure cannot reject an already accepted webhook", async () => {
  const errors = [], scheduled = [];
  const acceptor = createTelegramUpdateAcceptor({
    acceptUpdateId: async () => true,
    handleUpdate: async () => { throw Object.assign(new Error("Too Many Requests"), { token: "must-not-log" }); },
    logger: { error: (fields) => errors.push(fields) },
    schedule: (callback) => scheduled.push(callback),
  });
  assert.deepEqual(await acceptor.accept({ update_id: 99 }), { accepted: true, duplicate: false });
  scheduled[0](); await new Promise(setImmediate);
  assert.equal(errors.length, 1);
  assert.equal(errors[0].error_message, "Too Many Requests");
  assert.doesNotMatch(JSON.stringify(errors[0]), /must-not-log/);
});

test("malformed update is rejected before scheduling", async () => {
  let scheduled = false;
  const acceptor = createTelegramUpdateAcceptor({
    acceptUpdateId: async () => true, handleUpdate: async () => {}, schedule: () => { scheduled = true; },
  });
  assert.deepEqual(await acceptor.accept({ update_id: "bad" }), { accepted: false, invalid: true });
  assert.equal(scheduled, false);
});
