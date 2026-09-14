const url = process.env.OLLAMA_URL ?? "http://100.68.131.86:11435/api/generate";
const base = `<|startoftext|><|im_start|>system
Answer accurately and briefly.<|im_end|>
<|im_start|>user
What is 17 times 23?<|im_end|>
<|im_start|>assistant
`;
for (const variant of [
  { name: "native_raw", prompt: base },
  { name: "native_raw_think_false", prompt: base, think: false },
  { name: "closed_bridge", prompt: `${base}<think>\nNo unnecessary reasoning. Close thinking and answer immediately.\n</think>\n` },
]) {
  const response = await fetch(url, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({
    model: "newo-main", prompt: variant.prompt, raw: true, stream: false, keep_alive: -1,
    ...(variant.think === undefined ? {} : { think: variant.think }),
    options: { num_predict: 512, temperature: 0.2, top_k: 80, repeat_penalty: 1.05, seed: 42,
      stop: ["<|im_end|>", "<|im_start|>"] },
  }) });
  const result = await response.json();
  console.log(JSON.stringify({ name: variant.name, response: result.response, eval_count: result.eval_count,
    prompt_eval_count: result.prompt_eval_count, total_duration: result.total_duration }));
}
