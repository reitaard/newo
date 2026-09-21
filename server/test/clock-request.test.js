import test from "node:test";
import assert from "node:assert/strict";
import { formatClockReply, parseClockRequest } from "../src/clock-request.js";

const now = new Date("2026-09-21T03:00:00Z"); // 10:00 in Bangkok
const options = { now, timeZone: "Asia/Bangkok" };

test("spoken and numeric timer durations normalize deterministically", () => {
  const cases = [
    ["set a timer for ten minutes", 600],
    ["timer for twenty five minutes", 1500],
    ["set a timer for one hour thirty minutes", 5400],
    ["set a timer for half an hour", 1800],
    ["set a timer for an hour", 3600],
    ["set a timer for 1 hour 2 minutes", 3720],
    ["set a timer for 90 seconds", 90],
  ];
  for (const [text, duration_s] of cases)
    assert.deepEqual(parseClockRequest(text, options), { kind: "command", action: "create_timer", duration_s });
});

test("spoken alarms produce the same epochs as numeric equivalents", () => {
  const pairs = [
    ["set an alarm for seven AM tomorrow", "set an alarm for 7 AM tomorrow"],
    ["wake me at seven thirty AM tomorrow", "wake me at 7:30 AM tomorrow"],
    ["set an alarm for twelve PM tomorrow", "set an alarm for 12 PM tomorrow"],
    ["set an alarm at eleven forty five PM tomorrow", "set an alarm at 11:45 PM tomorrow"],
  ];
  for (const [spoken, numeric] of pairs) {
    const expected = parseClockRequest(numeric, options);
    assert.equal(expected.kind, "command");
    assert.deepEqual(parseClockRequest(spoken, options), expected);
  }
});

test("clear clock intents with missing, ambiguous, or invalid values ask instead of scheduling", () => {
  assert.deepEqual(parseClockRequest("set an alarm for seven", options),
    { kind: "ambiguous", message: "Please say AM or PM." });
  assert.deepEqual(parseClockRequest("wake me tomorrow", options),
    { kind: "ambiguous", message: "Please give an alarm time." });
  assert.deepEqual(parseClockRequest("set a timer for a little while", options),
    { kind: "ambiguous", message: "Please give the timer duration in hours, minutes, or seconds." });
  assert.deepEqual(parseClockRequest("set an alarm for 25:90", options),
    { kind: "ambiguous", message: "Please give a valid alarm time." });
});

test("unsupported date qualifiers fail closed, including day after tomorrow", () => {
  const requests = [
    "set an alarm for 7 PM Friday",
    "set an alarm at 6 PM next Monday",
    "wake me at 8 next week",
    "set an alarm for 9 AM September 25",
    "wake me at seven the day after tomorrow",
  ];
  for (const text of requests) assert.deepEqual(parseClockRequest(text, options),
    { kind: "ambiguous", message: "Please give a supported date such as today or tomorrow." });
  assert.equal(parseClockRequest("wake me at seven the day after tomorrow", options).kind, "ambiguous");
  assert.equal(parseClockRequest("wake me at seven AM tomorrow", options).kind, "command");
});

test("time and date discussion stays in the normal assistant pipeline", () => {
  for (const text of [
    "explain time dilation",
    "what time does the sun set",
    "what time is the football match",
    "what's the date of World War II",
    "tell me about date formats",
    "time complexity of quicksort",
    "is daylight saving time used in Japan",
  ]) assert.equal(parseClockRequest(text, options).kind, "not_clock", text);
});

test("positive clock phrases retain deterministic routing", () => {
  const actions = new Map([
    ["what time is it", "current_time"],
    ["tell me the time", "current_time"],
    ["what's today's date", "current_date"],
    ["pause the timer", "pause_timer"],
    ["resume the timer", "resume_timer"],
    ["start the stopwatch", "start_stopwatch"],
    ["snooze for five minutes", "snooze"],
    ["dismiss the alarm", "dismiss"],
  ]);
  for (const [text, action] of actions) assert.equal(parseClockRequest(text, options).action, action, text);
  assert.equal(parseClockRequest("snooze for five minutes", options).duration_s, 300);
});

test("confirmation is normalized and requires an accepted ACK", () => {
  const timer = parseClockRequest("set a timer for ninety seconds", options);
  // Ninety is deliberately outside the supported 1..59 spoken grammar.
  assert.equal(timer.kind, "ambiguous");
  const acceptedTimer = parseClockRequest("set a timer for one minute thirty seconds", options);
  assert.equal(formatClockReply(acceptedTimer, { applied: true }, options), "Timer set for 1 minute and 30 seconds.");
  assert.equal(formatClockReply(acceptedTimer, { applied: false, error: "persistence_failed" }, options),
    "The clock request was not accepted.");

  const alarm = parseClockRequest("set an alarm for seven AM tomorrow", options);
  const success = formatClockReply(alarm, { applied: true }, options);
  assert.match(success, /^Alarm set for /);
  assert.equal(formatClockReply(alarm, { applied: true, duplicate: true }, options), success);
  assert.doesNotMatch(formatClockReply(alarm, { applied: false }, options), /Alarm set/);
});
