import assert from "node:assert/strict";
import test from "node:test";
import { SpeechSession } from "../src/speech-session.js";

test("speech session emits bounded natural clauses from token-like chunks", () => {
  const output = [];
  const session = new SpeechSession({ onClause: (text) => output.push(text) });
  for (const chunk of ["Hello", ", I'm Newo", ". I can", " help you", " with that."]) session.pushText(chunk);
  session.finish();
  assert.deepEqual(output, ["Hello, I'm Newo.", "I can help you with that."]);
});

test("speech session is bounded and cancellation emits nothing further", () => {
  const output = []; const session = new SpeechSession({ maximumChars: 40, onClause: (text) => output.push(text) });
  session.pushText("one ".repeat(30));
  assert.ok(output.length > 0 && output.every((text) => text.length <= 90));
  session.cancel(); session.pushText("ignored."); session.finish();
  assert.ok(!output.includes("ignored."));
});
