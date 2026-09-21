import assert from "node:assert/strict";
import http from "node:http";
import test from "node:test";

import { listXiaomeiCommands, resolveXiaomeiCommand } from "../src/xiaomei-commands.js";
import { HyMtTranslationClient, XiaomeiModelClient } from "../src/xiaomei-clients.js";
import { createXiaomeiRuntime, routeXiaomeiTurn } from "../src/xiaomei-runtime.js";
import { createXiaomeiSession } from "../src/xiaomei-session.js";
import { Qwen3TtsBackend } from "../src/qwen3-tts.js";

function model(name, reply) {
  return { model: name, calls: [], async complete(request) { this.calls.push(request); return { text: reply, model: name, latencyMs: 2 }; } };
}
function translator(reply = "翻译") {
  return { model: "newo-translate:latest", calls: [], async translate(request) { this.calls.push(request);
    return { text: reply, model: this.model, latencyMs: 3 }; } };
}

test("Xiaomei session commands are centralized and state-sensitive", () => {
  const session = createXiaomeiSession();
  assert.equal(resolveXiaomeiCommand("玲玲", session).id, "session.activate");
  assert.equal(resolveXiaomeiCommand("pause", session), null);
  session.active = true; session.mode = "interpreter";
  assert.equal(resolveXiaomeiCommand("pause", session).id, "interpreter.pause");
  assert.ok(listXiaomeiCommands().length >= 15);
});

test("ordinary interpreter speech takes the direct Hy-MT2 route", async () => {
  const gemma = model("gemma", "unused"), hy = translator("I will not be home tomorrow morning at eight thirty.");
  const runtime = createXiaomeiRuntime({ gemma, translator: hy });
  await runtime.handleTranscript({ deviceId: "n", streamId: "1", text: "玲玲" }, { dryRun: true });
  await runtime.handleTranscript({ deviceId: "n", streamId: "2", text: "start translating" }, { dryRun: true });
  const result = await runtime.handleTranscript({ deviceId: "n", streamId: "3", text: "我明天上午八点半不在家。" }, { dryRun: true });
  assert.equal(result.route, "interpreter_translate");
  assert.equal(gemma.calls.length, 0);
  assert.deepEqual(hy.calls[0].sourceLanguage, "Chinese");
  assert.deepEqual(hy.calls[0].targetLanguage, "English");
  assert.equal(result.session.interpreter.lastSource, "我明天上午八点半不在家。");
});

test("interpreter meta uses recent translation context then remains in interpreter", async () => {
  const gemma = model("gemma", "上午 means morning before noon."), hy = translator("Tomorrow morning.");
  const runtime = createXiaomeiRuntime({ gemma, translator: hy });
  for (const [streamId, text] of [["1", "玲玲"], ["2", "start interpreter"], ["3", "明天上午"]])
    await runtime.handleTranscript({ deviceId: "n", streamId, text }, { dryRun: true });
  const meta = await runtime.handleTranscript({ deviceId: "n", streamId: "4", text: "What did she mean?" }, { dryRun: true });
  assert.equal(meta.route, "interpreter_meta");
  assert.match(gemma.calls[0].system, /明天上午/);
  assert.equal(meta.session.mode, "interpreter");
  const resumed = await runtime.handleTranscript({ deviceId: "n", streamId: "5", text: "See you later" }, { dryRun: true });
  assert.equal(resumed.route, "interpreter_translate");
});

test("pause resume stop and mode transitions are deterministic", async () => {
  const runtime = createXiaomeiRuntime({ gemma: model("gemma", "chat"), translator: translator() });
  const send = (text, id) => runtime.handleTranscript({ deviceId: "n", streamId: String(id), text }, { dryRun: true });
  await send("玲玲", 1); await send("interpreter mode", 2);
  assert.equal((await send("pause", 3)).session.interpreter.paused, true);
  assert.equal((await send("resume", 4)).session.interpreter.paused, false);
  const stopped = await send("stop translating", 5);
  assert.equal(stopped.session.mode, "chat"); assert.equal(stopped.session.interpreter.active, false);
});

test("one-shot translation selects language and teaching requests compose specialists", async () => {
  const gemma = model("gemma", "It is a natural, slightly reluctant phrasing."), hy = translator("我今晚不太想去。");
  const runtime = createXiaomeiRuntime({ gemma, translator: hy });
  await runtime.handleTranscript({ deviceId: "n", streamId: "1", text: "玲玲" }, { dryRun: true });
  const result = await runtime.handleTranscript({ deviceId: "n", streamId: "2", text: "How would I say ‘I don't really want to go tonight’ in Chinese?" }, { dryRun: true });
  assert.equal(result.route, "translate_once"); assert.equal(hy.calls.length, 1); assert.equal(gemma.calls.length, 1);
  assert.match(result.text, /我今晚不太想去/); assert.equal(result.direction.targetLanguage, "Chinese");
});

test("stale Xiaomei work is cancelled when a newer generation starts", async () => {
  let release;
  const gemma = { complete: ({ signal }) => new Promise((resolve, reject) => {
    release = () => resolve({ text: "late", model: "gemma", latencyMs: 50 });
    signal.addEventListener("abort", () => reject(signal.reason));
  }) };
  const runtime = createXiaomeiRuntime({ gemma, translator: translator() });
  await runtime.handleTranscript({ deviceId: "n", streamId: "1", text: "玲玲" }, { dryRun: true });
  const first = runtime.handleTranscript({ deviceId: "n", streamId: "2", text: "tell me something" }, { dryRun: true });
  await new Promise((resolve) => setImmediate(resolve));
  runtime.interruptDevice("n", "barge_in"); release();
  assert.equal((await first).kind, "cancelled");
});

test("Gemma and Hy-MT clients send non-reasoning OpenAI-compatible requests", async () => {
  const bodies = [];
  const fetchImpl = async (_url, request) => { bodies.push(JSON.parse(request.body)); return new Response(JSON.stringify({ choices: [{ message: { content: "ok" } }] }), { status: 200, headers: { "content-type": "application/json" } }); };
  const gemma = new XiaomeiModelClient({ baseUrl: "http://local", model: "newo-gemma-e2b:latest", fetchImpl });
  const hy = new HyMtTranslationClient({ baseUrl: "http://local", model: "newo-translate:latest", fetchImpl });
  await gemma.complete({ system: "small", user: "hello" });
  await hy.translate({ text: "no", sourceLanguage: "English", targetLanguage: "Chinese", preserveTone: true });
  assert.equal(bodies[0].stream, false); assert.equal("think" in bodies[0], false);
  assert.match(bodies[1].messages[0].content, /do not soften profanity/i);
});

test("Serena Qwen3 TTS streams PCM and carries explicit bilingual configuration", async () => {
  let requestBody;
  const server = http.createServer(async (request, response) => {
    let body = ""; for await (const chunk of request) body += chunk; requestBody = JSON.parse(body);
    response.writeHead(200, { "content-type": "application/octet-stream" }); response.end(Buffer.from([1, 2, 3, 4]));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  try {
    const backend = new Qwen3TtsBackend({ baseUrl: `http://127.0.0.1:${server.address().port}`, speaker: "Serena", language: "Auto" });
    const source = await backend.stream("你好, hello", { sampleRate: 24_000, channels: 1, bitsPerSample: 16 });
    const chunks = []; for await (const chunk of source.audio) chunks.push(chunk);
    assert.equal(Buffer.concat(chunks).length, 4); assert.equal(requestBody.voice, "Serena"); assert.equal(requestBody.language, "Auto");
  } finally { server.close(); }
});

test("ambiguous active speech falls through to Xiaomei chat, not generic routing", () => {
  const session = createXiaomeiSession(); session.active = true;
  assert.equal(routeXiaomeiTurn("Maybe we could go later", session).route, "chat");
});
