import assert from "node:assert/strict";
import test from "node:test";

import { XIAOMEI_CONTROLLER_SCHEMA, parseControllerOutput } from "../src/xiaomei-controller.js";
import { XiaomeiOllamaClient } from "../src/xiaomei-clients.js";
import { createXiaomeiRuntime, routeXiaomeiTurn } from "../src/xiaomei-runtime.js";
import { createXiaomeiSession } from "../src/xiaomei-session.js";

function ndjsonResponse(items) {
  const body = items.map((item) => `${JSON.stringify(item)}\n`).join("");
  return new Response(body, { status: 200, headers: { "content-type": "application/x-ndjson" } });
}

function model(name, reply) {
  return { model: name, calls: [], async complete(request) {
    this.calls.push(request);
    return { text: reply, model: name, latencyMs: 2, firstContentMs: 1 };
  } };
}

function translator(reply = "翻译") {
  return { model: "newo-translate:latest", calls: [], async translate(request) {
    this.calls.push(request);
    return { text: reply, model: this.model, latencyMs: 3, firstContentMs: 1 };
  } };
}

test("native Ollama client explicitly disables thinking and keeps schema/system prompt", async () => {
  let requestBody;
  const fetchImpl = async (_url, request) => {
    requestBody = JSON.parse(request.body);
    return ndjsonResponse([
      { message: { content: '{"route":"chat","reply":"hi"}' }, done: false },
      { done: true, eval_count: 4, load_duration: 0 },
    ]);
  };
  const client = new XiaomeiOllamaClient({ baseUrl: "http://local", model: "newo-gemma-e2b:latest", fetchImpl });
  await client.complete({
    system: "SYSTEM",
    messages: [{ role: "user", content: "hello" }],
    format: XIAOMEI_CONTROLLER_SCHEMA,
  });
  assert.equal(requestBody.think, false);
  assert.equal(requestBody.keep_alive, -1);
  assert.deepEqual(requestBody.format, XIAOMEI_CONTROLLER_SCHEMA);
  assert.equal(requestBody.messages[0].role, "system");
  assert.equal(requestBody.messages[0].content, "SYSTEM");
  assert.equal(requestBody.messages[1].content, "hello");
});

test("controller output parser enforces the seven-route contract", () => {
  assert.equal(parseControllerOutput('{"route":"interpreter_meta","reply":"ok"}').route, "interpreter_meta");
  assert.throws(() => parseControllerOutput('{"route":"made_up","reply":"ok"}'), /invalid Xiaomei controller output/);
});

test("broad interpreter-start language no longer falls straight to chat", () => {
  const session = createXiaomeiSession();
  session.active = true;
  const decision = routeXiaomeiTurn("I'm with a Chinese speaker now. Translate between us until I tell you to stop.", session);
  assert.equal(decision.route, "semantic");
});

test("reference translation routes as one-shot translation", () => {
  const session = createXiaomeiSession();
  session.active = true;
  session.lastTurn.assistantText = "You can take the metro; it should be faster.";
  assert.equal(routeXiaomeiTurn("Can you say that in Chinese?", session).route, "translate_once");
});

test("possible private interpreter meta question is not blindly translated", () => {
  const session = createXiaomeiSession();
  session.active = true;
  session.mode = "interpreter";
  session.interpreter.active = true;
  const decision = routeXiaomeiTurn("What did she mean by that? Explain it to me, don't translate this question to her.", session);
  assert.notEqual(decision.route, "interpreter_translate");
  assert.ok(["interpreter_meta", "semantic"].includes(decision.route));
});

test("ordinary interpreter speech keeps the direct Hy-MT2 hot path", async () => {
  const gemma = model("gemma", "unused");
  const hy = translator("I will not be home tomorrow morning at eight thirty.");
  const controller = { calls: [], async classify(request) { this.calls.push(request); return { route: "chat", reply: "", model: "gemma" }; } };
  const runtime = createXiaomeiRuntime({ gemma, translator: hy, controller });
  await runtime.handleTranscript({ deviceId: "n", streamId: "1", text: "玲玲" }, { dryRun: true });
  await runtime.handleTranscript({ deviceId: "n", streamId: "2", text: "start translating" }, { dryRun: true });
  const result = await runtime.handleTranscript({ deviceId: "n", streamId: "3", text: "我明天上午八点半不在家。" }, { dryRun: true });
  assert.equal(result.route, "interpreter_translate");
  assert.equal(controller.calls.length, 0);
  assert.equal(gemma.calls.length, 0);
  assert.equal(hy.calls.length, 1);
});

test("ambiguous interpreter meta uses semantic controller and remains private", async () => {
  const gemma = model("gemma", "上午 means morning before noon.");
  const hy = translator("Tomorrow morning.");
  const controller = { calls: [], async classify(request) {
    this.calls.push(request);
    return { route: "interpreter_meta", reply: "", model: "newo-gemma-e2b:latest", latencyMs: 2, firstContentMs: 1 };
  } };
  const runtime = createXiaomeiRuntime({ gemma, translator: hy, controller });
  await runtime.handleTranscript({ deviceId: "n", streamId: "1", text: "玲玲" }, { dryRun: true });
  await runtime.handleTranscript({ deviceId: "n", streamId: "2", text: "start translating" }, { dryRun: true });
  await runtime.handleTranscript({ deviceId: "n", streamId: "3", text: "明天上午" }, { dryRun: true });
  const result = await runtime.handleTranscript({ deviceId: "n", streamId: "4", text: "Wait, what did she mean by that exactly?" }, { dryRun: true });
  assert.equal(result.route, "interpreter_meta");
  assert.equal(controller.calls.length, 1);
  assert.equal(hy.calls.length, 1);
  assert.equal(gemma.calls.length, 1);
});
