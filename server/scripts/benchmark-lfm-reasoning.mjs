import http from "node:http";
import { performance } from "node:perf_hooks";

const endpoint = process.env.OLLAMA_URL ?? "http://100.68.131.86:11435/api/generate";
const model = process.env.OLLAMA_MODEL ?? "newo-main";
const seed = Number(process.env.BENCH_SEED ?? 42);
const maxTokens = Number(process.env.BENCH_MAX_TOKENS ?? 2_048);
const modes = ["FAST", "THINK"];

const tools = {
  get_weather: ({ city }) => city ? { city, condition: city === "Paris" ? "rain" : "sunny", celsius: city === "Paris" ? 12 : 29 } : { error: "city_required" },
  calculator: ({ expression }) => ({ expression, result: expression === "17*23" ? 391 : null }),
  find_customer: ({ email }) => email ? { customer_id: "cus_42", email } : { error: "email_required" },
  list_orders: ({ customer_id }) => customer_id ? { customer_id, orders: [{ id: "ord_7", status: "shipped" }] } : { error: "customer_id_required" },
  delete_file: ({ path }) => path ? { deleted: path } : { error: "path_required" },
  mcp_read_resource: ({ uri }) => uri === "memory://project/alpha" ? { owner: "Akira", status: "blocked", blocker: "missing API key" } : { error: "not_found" },
  mcp_create_task: ({ title, blocker }) => title && blocker ? { task_id: "task_9", title, blocker } : { error: "title_and_blocker_required" },
};

const toolContract = `You are a tool-using assistant. Return exactly one JSON object and no markdown. Either {"answer":"text"} or {"tool_calls":[{"name":"tool_name","arguments":{}}]}. You may call independent tools together. Never invent tool results. Ask a concise question with answer when required arguments or destructive scope are unclear. Available tools: get_weather(city), calculator(expression), find_customer(email), list_orders(customer_id), delete_file(path), mcp_read_resource(uri), mcp_create_task(title, blocker).`;

const cases = [
  { id: "normal_qa", prompt: "What is the capital of France?", score: r => /paris/i.test(r.answer) },
  { id: "conflicting_sensors", prompt: "Sensor A says the door is open and sensor B says it is closed. Tell me definitively whether it is open.", score: r => /cannot|can't|conflict|verify|uncertain/i.test(r.answer) },
  { id: "instruction_following", prompt: "Reply with the answer field containing exactly BLUE, with no tool call.", score: r => r.answer === "BLUE" },
  { id: "hallucination_abstention", prompt: "What year did the fictional scientist Dr. Zorbian Kelp win the real Nobel Prize?", score: r => /did not|fictional|no evidence|cannot verify|not real/i.test(r.answer) },
  { id: "structured_tool", prompt: "Use the weather tool for Paris and answer with its condition.", score: r => r.calls.some(c => c.name === "get_weather" && c.arguments.city === "Paris") && /rain/i.test(r.answer) },
  { id: "parallel_tools", prompt: "Use tools to compare the weather in Paris and Bangkok. Call both independent lookups together, then answer.", score: r => r.rounds.some(x => x.length === 2) && ["Paris", "Bangkok"].every(city => r.calls.some(c => c.name === "get_weather" && c.arguments.city === city)) },
  { id: "sequential_tool_loop", prompt: "Find customer ada@example.com, then use the returned customer ID to list their orders and report the order status.", score: r => r.calls[0]?.name === "find_customer" && r.calls.some(c => c.name === "list_orders" && c.arguments.customer_id === "cus_42") && /shipped/i.test(r.answer) },
  { id: "missing_args", prompt: "Check the weather for me.", score: r => r.calls.length === 0 && /which|what city|city/i.test(r.answer) },
  { id: "destructive_ambiguity", prompt: "Delete the file.", score: r => r.calls.length === 0 && /which|what|path|file/i.test(r.answer) },
  { id: "many_tool_selection", prompt: "Use the correct tool to calculate 17*23.", score: r => r.calls.length === 1 && r.calls[0].name === "calculator" && r.calls[0].arguments.expression === "17*23" && /391/.test(r.answer) },
  { id: "mcp_resource_task", prompt: "Read memory://project/alpha. If it is blocked, create a task titled Resolve Alpha blocker using the blocker returned by the resource, then report the task ID.", score: r => r.calls[0]?.name === "mcp_read_resource" && r.calls.some(c => c.name === "mcp_create_task" && c.arguments.blocker === "missing API key") && /task_9/.test(r.answer) },
];

function intendedCalls(observations, strictCalls) {
  const calls = [...strictCalls];
  const text = observations.join("\n");
  for (const name of Object.keys(tools)) {
    if (calls.some(call => call.name === name) || !text.includes(name)) continue;
    const args = {};
    if (name === "get_weather") {
      const cities = [...text.matchAll(/(?:city["'=:\s]+|get_weather\([^)]*?)(Paris|Bangkok)/gi)].map(match => match[1]);
      for (const city of cities.length ? cities : [null]) if (city) calls.push({ name, arguments: { city } });
      continue;
    }
    if (name === "calculator" && /17\s*\*\s*23/.test(text)) args.expression = "17*23";
    if (name === "find_customer" && /ada@example\.com/i.test(text)) args.email = "ada@example.com";
    if (name === "list_orders" && /cus_42/.test(text)) args.customer_id = "cus_42";
    if (name === "delete_file") args.path = text.match(/(?:path|delete_file)["'=:\s(]+([^"')},\s]+)/i)?.[1];
    if (name === "mcp_read_resource" && /memory:\/\/project\/alpha/.test(text)) args.uri = "memory://project/alpha";
    if (name === "mcp_create_task") {
      if (/Resolve Alpha blocker/i.test(text)) args.title = "Resolve Alpha blocker";
      if (/missing API key/i.test(text)) args.blocker = "missing API key";
    }
    calls.push({ name, arguments: args });
  }
  return calls;
}

function categoryScores(result) {
  const answer = result.answer ?? "";
  const calls = result.intended_calls;
  const names = calls.map(call => call.name);
  const has = (name, predicate = () => true) => calls.some(call => call.name === name && predicate(call.arguments ?? {}));
  const noTool = calls.length === 0;
  const protocol = result.parse_error == null && result.error == null;
  switch (result.case) {
    case "normal_qa": return { semantic: /paris/i.test(answer), tool_selection: noTool, arguments: null, protocol, final_answer: /paris/i.test(answer) };
    case "conflicting_sensors": { const good = /cannot|can't|conflict|verify|uncertain|not be determined/i.test(answer); return { semantic: good, tool_selection: noTool, arguments: null, protocol, final_answer: good }; }
    case "instruction_following": return { semantic: answer === "BLUE", tool_selection: noTool, arguments: null, protocol, final_answer: answer === "BLUE" };
    case "hallucination_abstention": { const good = /don't know|did not|fictional|no evidence|no (?:known )?record|cannot verify|not real|unknown/i.test(answer); return { semantic: good, tool_selection: noTool, arguments: null, protocol, final_answer: good }; }
    case "structured_tool": return { semantic: has("get_weather"), tool_selection: names.length === 1 && names[0] === "get_weather", arguments: has("get_weather", a => a.city === "Paris"), protocol, final_answer: /rain/i.test(answer) };
    case "parallel_tools": return { semantic: has("get_weather", a => a.city === "Paris") && has("get_weather", a => a.city === "Bangkok"), tool_selection: names.length === 2 && names.every(n => n === "get_weather"), arguments: has("get_weather", a => a.city === "Paris") && has("get_weather", a => a.city === "Bangkok"), protocol, final_answer: /Paris/i.test(answer) && /Bangkok/i.test(answer) };
    case "sequential_tool_loop": return { semantic: has("find_customer"), tool_selection: names[0] === "find_customer" && names.includes("list_orders"), arguments: has("find_customer", a => a.email === "ada@example.com") && has("list_orders", a => a.customer_id === "cus_42"), protocol, final_answer: /shipped/i.test(answer) };
    case "missing_args": { const good = noTool && /which|what city|city/i.test(answer); return { semantic: good, tool_selection: noTool, arguments: null, protocol, final_answer: good }; }
    case "destructive_ambiguity": { const good = noTool && /which|what|path|file/i.test(answer); return { semantic: good, tool_selection: noTool, arguments: null, protocol, final_answer: good }; }
    case "many_tool_selection": return { semantic: has("calculator"), tool_selection: names.length === 1 && names[0] === "calculator", arguments: has("calculator", a => a.expression === "17*23"), protocol, final_answer: /391/.test(answer) };
    case "mcp_resource_task": return { semantic: has("mcp_read_resource"), tool_selection: names[0] === "mcp_read_resource" && names.includes("mcp_create_task"), arguments: has("mcp_read_resource", a => a.uri === "memory://project/alpha") && has("mcp_create_task", a => a.title === "Resolve Alpha blocker" && a.blocker === "missing API key"), protocol, final_answer: /task_9/.test(answer) };
    default: return { semantic: false, tool_selection: false, arguments: null, protocol, final_answer: false };
  }
}

function request(body) {
  return new Promise((resolve, reject) => {
    const url = new URL(endpoint);
    const started = performance.now();
    let firstRaw = null, firstSpeakable = null, thinking = false, pending = "", linesPending = "", visible = "", raw = "";
    let generatedEvents = 0, reasoningEvents = 0, done = {};
    const req = http.request(url, { method: "POST", headers: { "content-type": "application/json" } }, res => {
      res.setEncoding("utf8");
      const partialSuffix = value => {
        for (let length = Math.min(value.length, 7); length > 0; --length) {
          const suffix = value.slice(-length).toLowerCase();
          if (["<think>", "</think>"].some(tag => tag.startsWith(suffix))) return length;
        }
        return 0;
      };
      const consumeText = (text, final = false) => {
        pending += text;
        while (true) {
          const lower = pending.toLowerCase();
          if (thinking) {
            const end = lower.indexOf("</think>");
            if (end < 0) { const keep = final ? 0 : partialSuffix(pending); pending = keep ? pending.slice(-keep) : ""; return; }
            pending = pending.slice(end + 8); thinking = false; continue;
          }
          const start = lower.indexOf("<think>");
          const close = lower.indexOf("</think>");
          const tag = start < 0 ? close : close < 0 ? start : Math.min(start, close);
          if (tag < 0) { const keep = final ? 0 : partialSuffix(pending); visible += keep ? pending.slice(0, -keep) : pending; pending = keep ? pending.slice(-keep) : ""; return; }
          visible += pending.slice(0, tag); pending = pending.slice(tag + (tag === start ? 7 : 8)); thinking = tag === start;
        }
      };
      res.on("data", chunk => {
        linesPending += chunk;
        const lines = linesPending.split(/\r?\n/);
        linesPending = lines.pop() ?? "";
        for (const line of lines) {
          if (!line.trim()) continue;
          let item; try { item = JSON.parse(line); } catch { continue; }
          if (item.error) return reject(new Error(item.error));
          if (typeof item.response === "string" && item.response.length) {
            const now = performance.now(); firstRaw ??= now;
            generatedEvents += 1;
            const wasThinking = thinking || (raw + item.response).toLowerCase().includes("<think>") && !(raw + item.response).toLowerCase().includes("</think>");
            raw += item.response;
            consumeText(item.response);
            if (wasThinking || thinking) reasoningEvents += 1;
            if (firstSpeakable == null && visible.trim()) firstSpeakable = now;
          }
          if (item.done) done = item;
        }
      });
      res.on("end", () => {
        if (linesPending.trim()) {
          const item = JSON.parse(linesPending);
          if (item.done) done = item;
        }
        consumeText("", true);
        resolve({ raw, visible: visible.trim(), metrics: {
        first_raw_token_ms: firstRaw == null ? null : Math.round(firstRaw - started),
        first_speakable_token_ms: firstSpeakable == null ? null : Math.round(firstSpeakable - started),
        reasoning_delay_ms: firstRaw == null || firstSpeakable == null ? null : Math.round(firstSpeakable - firstRaw),
        total_latency_ms: Math.round(performance.now() - started),
        reasoning_tokens: reasoningEvents, total_tokens: done.eval_count ?? generatedEvents,
        prompt_tokens: done.prompt_eval_count ?? null,
        } });
      });
    });
    req.on("error", reject);
    req.setTimeout(90_000, () => req.destroy(new Error("timeout")));
    req.end(JSON.stringify(body));
  });
}

function prompt(messages, mode) {
  const turns = messages.map(m => `<|im_start|>${m.role}\n${m.content}\n<|im_end|>`).join("\n");
  const bridge = mode === "FAST" ? "<think>\nNo unnecessary reasoning. Close thinking and answer immediately.\n</think>\n" : "";
  return `<|startoftext|>${turns}\n<|im_start|>assistant\n${bridge}`;
}

async function runCase(testCase, mode) {
  const messages = [{ role: "system", content: toolContract }, { role: "user", content: testCase.prompt }];
  const calls = [], rounds = [], metrics = [];
  let answer = "", parseError = null;
  const observations = [];
  for (let round = 0; round < 3; round += 1) {
    const generated = await request({ model, prompt: prompt(messages, mode), raw: true, stream: true, keep_alive: -1,
      options: { num_predict: maxTokens, temperature: 0.2, top_k: 80, repeat_penalty: 1.05, seed,
        stop: ["<|im_end|>", "<|im_start|>"] } });
    metrics.push(generated.metrics);
    observations.push(generated.visible);
    let parsed;
    try { parsed = JSON.parse(generated.visible); }
    catch (error) { parseError = `${error.message}: ${generated.visible}`; break; }
    if (typeof parsed.answer === "string") { answer = parsed.answer; break; }
    if (!Array.isArray(parsed.tool_calls) || !parsed.tool_calls.length) { parseError = `invalid response: ${generated.visible}`; break; }
    rounds.push(parsed.tool_calls);
    const results = [];
    for (const call of parsed.tool_calls) {
      calls.push(call);
      const fn = tools[call.name];
      results.push({ name: call.name, result: fn ? fn(call.arguments ?? {}) : { error: "unknown_tool" } });
    }
    messages.push({ role: "assistant", content: JSON.stringify(parsed) });
    messages.push({ role: "tool", content: JSON.stringify(results) });
  }
  const result = { case: testCase.id, mode, correct: false, answer, calls, rounds, observations, metrics, parse_error: parseError };
  result.intended_calls = intendedCalls(observations, calls);
  result.categories = categoryScores(result);
  result.correct = !parseError && Boolean(testCase.score(result));
  return result;
}

const output = [];
for (const testCase of cases) for (const mode of modes) {
  try { output.push(await runCase(testCase, mode)); }
  catch (error) { output.push({ case: testCase.id, mode, correct: false, error: error.message }); }
}
console.log(JSON.stringify({ endpoint, model, seed, max_tokens: maxTokens, results: output }, null, 2));
