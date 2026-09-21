import test from "node:test";
import assert from "node:assert/strict";
import { formatClockReply, parseClockRequest } from "../src/clock-request.js";

const now = new Date("2026-09-21T03:00:00Z"); // 10:00 in Bangkok
const options = { now, timeZone: "Asia/Bangkok" };

test("separates relative timers from absolute alarms", () => {
  assert.deepEqual(parseClockRequest("set a timer for 1 hour 2 minutes", options),
    { kind: "command", action: "create_timer", duration_s: 3720 });
  assert.deepEqual(parseClockRequest("set an alarm at 7 AM tomorrow", options),
    { kind: "command", action: "create_alarm", epoch_s: 1790035200 });
});

test("does not guess ambiguous absolute times", () => {
  assert.deepEqual(parseClockRequest("set an alarm for 7", options),
    { kind: "ambiguous", message: "Please say AM or PM." });
  assert.equal(parseClockRequest("tell me a joke", options).kind, "not_clock");
});

test("parses control and stopwatch actions", () => {
  assert.equal(parseClockRequest("pause the timer", options).action, "pause_timer");
  assert.equal(parseClockRequest("snooze for 5 minutes", options).duration_s, 300);
  assert.equal(parseClockRequest("start the stopwatch", options).action, "start_stopwatch");
  assert.equal(parseClockRequest("stop", options).action, "dismiss");
});

test("confirmation uses normalized values only after an accepted ack", () => {
  const request = parseClockRequest("set a timer for 90 seconds", options);
  assert.equal(formatClockReply(request, { applied: true }, options), "Timer set for 1 minute and 30 seconds.");
  assert.equal(formatClockReply(request, { applied: false, error: "persistence_failed" }, options),
    "The clock request was not accepted.");
});
