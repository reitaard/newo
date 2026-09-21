import { createXiaomeiRuntime } from "../src/xiaomei-runtime.js";

const gemma = { async complete({ user }) { return { text: `Gemma: ${user}`, model: "newo-gemma-e2b:latest", latencyMs: 1 }; } };
const translator = { async translate({ text, sourceLanguage, targetLanguage }) {
  return { text: `[${sourceLanguage}→${targetLanguage}] ${text}`, model: "newo-translate:latest", latencyMs: 1 };
} };
const runtime = createXiaomeiRuntime({ gemma, translator });
const sequence = ["玲玲", "你好，我想练一下中文。", "How do I say ‘I don't really want to go tonight’ in Chinese?",
  "Start translating between us", "Hello, nice to meet you", "我明天上午八点半不在家。", "What did she mean?", "See you tomorrow", "Stop translating"];
for (const [index, text] of sequence.entries()) {
  const result = await runtime.handleTranscript({ deviceId: "diagnostic", streamId: String(index + 1), text }, { dryRun: true });
  console.log(JSON.stringify({ input: text, route: result.route, model: result.model, output: result.text,
    timing: result.timing, mode: result.session?.mode, interpreter: result.session?.interpreter }, null, 2));
}
