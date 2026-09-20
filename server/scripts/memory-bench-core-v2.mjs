import http from "node:http";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { performance } from "node:perf_hooks";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DEFAULT_DATASET = path.resolve(HERE, "../benchmarks/memory/newo-gold-v1.json");
const RESULTS_DIR = path.resolve(HERE, "../benchmarks/memory/results");

const endpoint = process.env.OLLAMA_URL ?? "http://100.110.136.15:11435/api/generate";
const model = process.env.OLLAMA_MODEL ?? "newo-main";
const seed = Number(process.env.BENCH_SEED ?? 42);
const maxTokens = Number(process.env.BENCH_MAX_TOKENS ?? 256);
const limit = Number(process.env.BENCH_LIMIT ?? 0);
const structured = process.env.BENCH_STRUCTURED !== "0";
const modes = String(process.env.BENCH_MODES ?? "FAST,THINK")
  .split(",").map(x => x.trim().toUpperCase()).filter(Boolean);

const MEMORY_TYPES = ["preference", "fact", "relationship", "device", "location", "event", "plan"];

// Both prompt variants MUST use the same response contract. This keeps the benchmark
// about extraction policy rather than allowing one variant a simpler output shape.
const MEMORY_SCHEMA = {
  type: "object",
  additionalProperties: false,
  properties: {
    memory: {
      type: "array",
      items: {
        type: "object",
        additionalProperties: false,
        properties: {
          text: { type: "string" },
          type: { type: "string", enum: MEMORY_TYPES },
          event_at: { anyOf: [{ type: "string" }, { type: "null" }] },
        },
        required: ["text", "type", "event_at"],
      },
    },
  },
  required: ["memory"],
};

const COMMON_CONTRACT = `Return exactly one JSON object with top-level "memory" array. Each memory item has exactly: text, type, event_at. type must be one of ${MEMORY_TYPES.join("|")}. event_at is an ISO-8601 date/time or null. Empty memory is valid.`;

const MINIMAL_PROMPT = `You extract durable memories for a private voice assistant. ${COMMON_CONTRACT} Extract only facts useful in a future conversation: stable preferences, personal facts, relationships, device/location facts, meaningful events, and explicit plans. Do not store the current request, small talk, assistant output, temporary actions, guesses, or uncertain speculation. Split independent facts into separate memories. Preserve the user's language. Prefer an empty memory over junk.`;

const VOICEMEM_STYLE_PROMPT = `You are an additive long-term memory extractor for live voice conversations. ${COMMON_CONTRACT} Capture durable user/world knowledge as atomic memories without replacing unrelated prior facts. Resolve explicit relative dates from the supplied reference time when possible. Preserve the user's language and proper nouns. Do NOT remember audibility or connection checks, identity probes with no new fact, remarks about the conversation, garbled ASR fragments, assistant suggestions or answers, one-off requests, or statements whose only content is that something was asked or said. Do not turn uncertainty such as maybe/perhaps/might into a fact. Test each candidate with: would this still help a week from now in a different conversation? If not, omit it.`;

function systemPrompt(style) {
  if (style === "minimal") return MINIMAL_PROMPT;
  if (style === "voicemem") return VOICEMEM_STYLE_PROMPT;
  throw new Error(`unknown benchmark style: ${style}`);
}

function buildPrompt(style, testCase, referenceNow, mode) {
  const context = Array.isArray(testCase.context) && testCase.context.length
    ? `Prior conversation context:\n${testCase.context.map(x => `${x.role}: ${x.text}`).join("\n")}\n\n`
    : "";
  const user = `Reference time: ${referenceNow}\n${context}Current user transcript:\n${testCase.input}`;
  const bridge = mode === "FAST"
    ? "<think>\nNo unnecessary reasoning. Close thinking and answer immediately.\n</think>\n"
    : "";
  return `<|startoftext|><|im_start|>system\n${systemPrompt(style)}\n<|im_end|>\n<|im_start|>user\n${user}\n<|im_end|>\n<|im_start|>assistant\n${bridge}`;
}

function stripThinking(text) {
  return String(text ?? "").replace(/<think>[\s\S]*?<\/think>/gi, "").trim();
}

function extractJson(text) {
  let value = stripThinking(text);
  const fenced = value.match(/^```(?:json)?\s*([\s\S]*?)\s*```$/i);
  if (fenced) value = fenced[1].trim();
  const start = value.indexOf("{");
  const end = value.lastIndexOf("}");
  if (start >= 0 && end > start) value = value.slice(start, end + 1);
  return JSON.parse(value);
}

function parseMemories(raw) {
  const parsed = extractJson(raw);
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed) || !Array.isArray(parsed.memory)) {
    throw new Error('response missing top-level "memory" array');
  }
  return parsed.memory.map((item, index) => {
    if (!item || typeof item !== "object" || Array.isArray(item)) throw new Error(`memory[${index}] is not an object`);
    const keys = Object.keys(item).sort();
    const wantedKeys = ["event_at", "text", "type"];
    if (keys.length !== wantedKeys.length || keys.some((key, i) => key !== wantedKeys[i])) {
      throw new Error(`memory[${index}] must contain exactly text,type,event_at`);
    }
    if (typeof item.text !== "string" || !item.text.trim()) throw new Error(`memory[${index}].text invalid`);
    if (!MEMORY_TYPES.includes(item.type)) throw new Error(`memory[${index}].type invalid: ${item.type}`);
    if (!(item.event_at === null || typeof item.event_at === "string")) throw new Error(`memory[${index}].event_at invalid`);
    return { text: item.text.trim(), type: item.type, event_at: item.event_at };
  });
}

function request(body) {
  return new Promise((resolve, reject) => {
    const url = new URL(endpoint);
    const started = performance.now();
    let firstRaw = null;
    let pending = "";
    let raw = "";
    let done = {};
    const req = http.request(url, { method: "POST", headers: { "content-type": "application/json" } }, res => {
      res.setEncoding("utf8");
      if ((res.statusCode ?? 500) < 200 || (res.statusCode ?? 500) >= 300) {
        let bodyText = "";
        res.on("data", chunk => { bodyText += chunk; });
        res.on("end", () => reject(new Error(`HTTP ${res.statusCode}: ${bodyText.slice(0, 300)}`)));
        return;
      }
      res.on("data", chunk => {
        pending += chunk;
        const lines = pending.split(/\r?\n/);
        pending = lines.pop() ?? "";
        for (const line of lines) {
          if (!line.trim()) continue;
          let item;
          try { item = JSON.parse(line); } catch { continue; }
          if (item.error) return reject(new Error(String(item.error)));
          if (typeof item.response === "string" && item.response.length) {
            firstRaw ??= performance.now();
            raw += item.response;
          }
          if (item.done) done = item;
        }
      });
      res.on("end", () => {
        if (pending.trim()) {
          try {
            const item = JSON.parse(pending);
            if (item.error) return reject(new Error(String(item.error)));
            if (typeof item.response === "string") {
              if (item.response.length) firstRaw ??= performance.now();
              raw += item.response;
            }
            if (item.done) done = item;
          } catch (error) {
            if (!raw) return reject(new Error(`invalid Ollama response: ${error.message}`));
          }
        }
        const evalNs = Number(done.eval_duration ?? 0);
        resolve({ raw, metrics: {
          first_raw_token_ms: firstRaw == null ? null : Math.round(firstRaw - started),
          total_latency_ms: Math.round(performance.now() - started),
          prompt_tokens: Number.isFinite(done.prompt_eval_count) ? done.prompt_eval_count : null,
          output_tokens: Number.isFinite(done.eval_count) ? done.eval_count : null,
          load_ms: Number.isFinite(done.load_duration) ? Number((done.load_duration / 1e6).toFixed(2)) : null,
          prompt_eval_ms: Number.isFinite(done.prompt_eval_duration) ? Number((done.prompt_eval_duration / 1e6).toFixed(2)) : null,
          eval_ms: evalNs ? Number((evalNs / 1e6).toFixed(2)) : null,
          tokens_per_s: evalNs && Number.isFinite(done.eval_count)
            ? Number((done.eval_count / (evalNs / 1e9)).toFixed(2)) : null,
        } });
      });
    });
    req.on("error", reject);
    req.setTimeout(90_000, () => req.destroy(new Error("timeout")));
    req.end(JSON.stringify(body));
  });
}

function normalize(value) {
  return String(value ?? "").toLowerCase().normalize("NFKC")
    .replace(/[^\p{L}\p{N}]+/gu, " ").trim();
}

function expectedAlternatives(wanted) {
  if (Array.isArray(wanted.alternatives) && wanted.alternatives.length) return wanted.alternatives;
  return [wanted.terms ?? []];
}

function textMatches(wanted, actualText) {
  const actual = normalize(actualText);
  return expectedAlternatives(wanted).some(group => {
    const terms = group.map(normalize).filter(Boolean);
    return terms.length > 0 && terms.every(term => actual.includes(term));
  });
}

function scoreCase(testCase, memories) {
  const expected = Array.isArray(testCase.expected) ? testCase.expected : [];
  const unmatched = new Set(memories.map((_, i) => i));
  const matches = [];
  for (const wanted of expected) {
    let found = -1;
    for (const i of unmatched) {
      if (textMatches(wanted, memories[i].text)) { found = i; break; }
    }
    if (found >= 0) {
      unmatched.delete(found);
      const got = memories[found];
      matches.push({
        expected: wanted,
        actual: got,
        type_correct: wanted.type == null ? null : got.type === String(wanted.type).toLowerCase(),
        time_correct: Object.hasOwn(wanted, "event_at") ? (got.event_at ?? null) === wanted.event_at : null,
      });
    }
  }
  const matched = matches.length;
  const precision = memories.length ? matched / memories.length : expected.length ? 0 : 1;
  const recall = expected.length ? matched / expected.length : memories.length ? 0 : 1;
  return {
    expected_count: expected.length,
    extracted_count: memories.length,
    decision_correct: (expected.length > 0) === (memories.length > 0),
    count_correct: expected.length === memories.length,
    matched,
    false_positive_count: unmatched.size,
    precision,
    recall,
    f1: precision + recall ? 2 * precision * recall / (precision + recall) : 0,
    type_checks: matches.filter(x => x.type_correct != null).map(x => x.type_correct),
    time_checks: matches.filter(x => x.time_correct != null).map(x => x.time_correct),
    matches,
  };
}

function percentile(values, p) {
  const sorted = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!sorted.length) return null;
  const index = Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1));
  return sorted[index];
}

function average(values) {
  const usable = values.filter(Number.isFinite);
  return usable.length ? usable.reduce((a, b) => a + b, 0) / usable.length : null;
}

function aggregate(rows) {
  const valid = rows.filter(x => x.json_valid);
  const scores = valid.map(x => x.score);
  const requestErrors = rows.filter(x => x.error);
  const parseErrors = rows.filter(x => !x.error && !x.json_valid);
  const sum = key => scores.reduce((n, x) => n + Number(Boolean(x[key])), 0);
  const matched = scores.reduce((n, x) => n + x.matched, 0);
  const extracted = scores.reduce((n, x) => n + x.extracted_count, 0);
  const expected = scores.reduce((n, x) => n + x.expected_count, 0);
  const precision = valid.length ? (extracted ? matched / extracted : expected ? 0 : 1) : null;
  const recall = valid.length ? (expected ? matched / expected : extracted ? 0 : 1) : null;
  const typeChecks = scores.flatMap(x => x.type_checks);
  const timeChecks = scores.flatMap(x => x.time_checks);
  const totalLat = valid.map(x => x.metrics?.total_latency_ms);
  const ttft = valid.map(x => x.metrics?.first_raw_token_ms);
  return {
    cases: rows.length,
    valid_cases: valid.length,
    request_errors: requestErrors.length,
    parse_errors: parseErrors.length,
    json_valid_rate: rows.length ? valid.length / rows.length : 0,
    decision_accuracy: valid.length ? sum("decision_correct") / valid.length : null,
    exact_count_accuracy: valid.length ? sum("count_correct") / valid.length : null,
    fact_precision: precision,
    fact_recall: recall,
    fact_f1: precision == null || recall == null ? null : (precision + recall ? 2 * precision * recall / (precision + recall) : 0),
    false_memories: valid.length ? scores.reduce((n, x) => n + x.false_positive_count, 0) : null,
    type_accuracy: typeChecks.length ? typeChecks.filter(Boolean).length / typeChecks.length : null,
    temporal_accuracy: timeChecks.length ? timeChecks.filter(Boolean).length / timeChecks.length : null,
    ttft_ms: { p50: percentile(ttft, 50), p95: percentile(ttft, 95), mean: average(ttft) },
    total_latency_ms: { p50: percentile(totalLat, 50), p95: percentile(totalLat, 95), mean: average(totalLat) },
    mean_prompt_tokens: average(valid.map(x => x.metrics?.prompt_tokens)),
    mean_output_tokens: average(valid.map(x => x.metrics?.output_tokens)),
    mean_tokens_per_s: average(valid.map(x => x.metrics?.tokens_per_s)),
    first_error: requestErrors[0]?.error ?? parseErrors[0]?.parse_error ?? null,
  };
}

export async function runBenchmark(style) {
  const datasetPath = path.resolve(process.env.BENCH_DATASET ?? DEFAULT_DATASET);
  const dataset = JSON.parse(await readFile(datasetPath, "utf8"));
  const referenceNow = dataset.reference_now ?? new Date().toISOString();
  const sourceCases = Array.isArray(dataset.cases) ? dataset.cases : [];
  const cases = limit > 0 ? sourceCases.slice(0, limit) : sourceCases;
  const runs = [];

  console.log(`\nMemory benchmark v2: style=${style} endpoint=${endpoint} model=${model} cases=${cases.length} structured=${structured}`);
  for (const mode of modes) {
    const rows = [];
    for (const testCase of cases) {
      const started = new Date().toISOString();
      try {
        const body = {
          model,
          prompt: buildPrompt(style, testCase, referenceNow, mode),
          raw: true,
          stream: true,
          keep_alive: -1,
          options: {
            num_predict: maxTokens,
            temperature: 0,
            top_k: 80,
            repeat_penalty: 1.05,
            seed,
            stop: ["<|im_end|>", "<|im_start|>"],
          },
        };
        if (structured) body.format = MEMORY_SCHEMA;
        const generated = await request(body);
        let memories = [];
        let parseError = null;
        try { memories = parseMemories(generated.raw); } catch (error) { parseError = error.message; }
        rows.push({
          id: testCase.id,
          category: testCase.category,
          started_at: started,
          input: testCase.input,
          expected: testCase.expected,
          output: generated.raw,
          memories,
          json_valid: parseError == null,
          parse_error: parseError,
          score: parseError == null ? scoreCase(testCase, memories) : null,
          metrics: generated.metrics,
        });
      } catch (error) {
        rows.push({
          id: testCase.id,
          category: testCase.category,
          started_at: started,
          input: testCase.input,
          expected: testCase.expected,
          output: null,
          memories: [],
          json_valid: false,
          parse_error: null,
          error: error.message,
          score: null,
          metrics: null,
        });
      }
      const last = rows.at(-1);
      const detail = last.error ? ` request=${last.error}` : last.parse_error ? ` parse=${last.parse_error}` : "";
      console.log(`${style}/${mode} ${testCase.id}: ${last.json_valid ? "ok" : "ERR"} ${last.metrics?.total_latency_ms ?? "-"}ms${detail}`);
    }
    runs.push({ style, mode, structured, summary: aggregate(rows), rows });
  }

  await mkdir(RESULTS_DIR, { recursive: true });
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  const outputPath = path.join(RESULTS_DIR, `${stamp}-${style}-v2.json`);
  const report = {
    generated_at: new Date().toISOString(),
    endpoint,
    model,
    seed,
    max_tokens: maxTokens,
    structured,
    dataset: dataset.name ?? path.basename(datasetPath),
    reference_now: referenceNow,
    style,
    runs,
  };
  await writeFile(outputPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");
  console.log(`\nSaved ${outputPath}`);
  for (const run of runs) console.log(`${run.style}/${run.mode}`, run.summary);
  return report;
}
