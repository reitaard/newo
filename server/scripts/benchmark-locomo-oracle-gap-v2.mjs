import http from "node:http";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { performance } from "node:perf_hooks";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const MEMORY_DIR = path.resolve(HERE, "../benchmarks/memory");
const CACHE_DIR = path.join(MEMORY_DIR, "cache");
const RESULTS_DIR = path.join(MEMORY_DIR, "results");
const CACHE_PATH = path.join(CACHE_DIR, "locomo10.json");

const LOCOMO_SOURCE = process.env.LOCOMO_URL
  ?? "https://raw.githubusercontent.com/snap-research/locomo/3eb6f2c585f5e1699204e3c3bdf7adc5c28cb376/data/locomo10.json";
const ollamaBase = String(process.env.LOCOMO_OLLAMA_BASE_URL
  ?? process.env.ASSISTANT_BASE_URL
  ?? "http://100.110.136.15:11435").replace(/\/+$/, "");
const endpoint = process.env.LOCOMO_OLLAMA_URL ?? `${ollamaBase}/api/generate`;
const model = process.env.LOCOMO_MODEL ?? process.env.OLLAMA_MODEL ?? "newo-main";
const perCategory = Math.max(1, Number(process.env.LOCOMO_PER_CATEGORY ?? 10));
const seed = Number(process.env.LOCOMO_SEED ?? 42);
const answerMaxTokens = Number(process.env.LOCOMO_ANSWER_MAX_TOKENS ?? 48);
const extractMaxTokens = Number(process.env.LOCOMO_EXTRACT_MAX_TOKENS ?? 256);
const refresh = process.env.LOCOMO_REFRESH === "1";
const categoryIds = String(process.env.LOCOMO_CATEGORIES ?? "4,1,2")
  .split(",").map(x => Number(x.trim())).filter(Number.isFinite);
const lanes = new Set(String(process.env.LOCOMO_LANES ?? "oracle,minimal,voicemem")
  .split(",").map(x => x.trim().toLowerCase()).filter(Boolean));

const CATEGORY_NAMES = new Map([
  [4, "single_hop"],
  [1, "multi_hop"],
  [2, "temporal"],
  [3, "open_domain"],
  [5, "adversarial"],
]);

const MEMORY_SCHEMA = {
  type: "object",
  additionalProperties: false,
  properties: {
    memory: {
      type: "array",
      items: {
        type: "object",
        additionalProperties: false,
        properties: { text: { type: "string" } },
        required: ["text"],
      },
    },
  },
  required: ["memory"],
};

const ANSWER_SCHEMA = {
  type: "object",
  additionalProperties: false,
  properties: { answer: { type: "string" } },
  required: ["answer"],
};

const MINIMAL_EXTRACTOR = `You compress conversation evidence into durable memory facts for later question answering. Return exactly one JSON object with top-level "memory" array; each item contains only "text". Capture only facts actually asserted in the evidence. Make every memory self-contained: resolve pronouns to named entities when the evidence permits it. Preserve names, dates, numbers, relationships, locations, preferences, meaningful events, and explicit plans. If a statement uses a relative date such as yesterday/tomorrow, resolve it from that turn's session timestamp when possible. Ignore greetings, questions, conversational filler, and unsupported inference. Split independent facts into separate memories. Empty memory is valid.`;

const VOICEMEM_EXTRACTOR = `You are an additive long-term memory extractor for conversation evidence. Return exactly one JSON object with top-level "memory" array; each item contains only "text". Store durable user/world knowledge useful in another conversation. Memories must be atomic and self-contained; resolve pronouns to named entities when possible. Preserve proper nouns, dates, numbers, relationships, locations, significant events, and explicit plans. Resolve relative dates such as yesterday/tomorrow from that turn's session timestamp when possible. Do not store greetings, questions, one-off requests, conversation-meta remarks, assistant-like suggestions, uncertain speculation, or statements whose only content is that something was asked or said. Do not invent facts. Empty memory is valid and preferred over junk.`;

const ANSWER_SYSTEM = `You answer a benchmark question from supplied evidence. Return exactly one JSON object with one field: "answer". The answer value must contain only the shortest answer that fully answers the question—never repeat the evidence, question, labels, or instructions. Use only supplied evidence. Resolve relative time expressions such as yesterday/tomorrow using the timestamp attached to that evidence turn. For a list answer, use a concise comma-separated list. Preserve names, dates and numbers. If evidence is insufficient, answer exactly "Not mentioned".`;

function fastPrompt(system, user) {
  return `<|startoftext|><|im_start|>system\n${system}\n<|im_end|>\n<|im_start|>user\n${user}\n<|im_end|>\n<|im_start|>assistant\n<think>\nNo unnecessary reasoning. Close thinking and answer immediately.\n</think>\n`;
}

function percentile(values, p) {
  const usable = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!usable.length) return null;
  const i = Math.min(usable.length - 1, Math.max(0, Math.ceil((p / 100) * usable.length) - 1));
  return usable[i];
}

function average(values) {
  const usable = values.filter(Number.isFinite);
  return usable.length ? usable.reduce((a, b) => a + b, 0) / usable.length : null;
}

async function fetchDataset() {
  const override = process.env.LOCOMO_DATASET;
  if (override) return JSON.parse(await readFile(path.resolve(override), "utf8"));
  await mkdir(CACHE_DIR, { recursive: true });
  if (!refresh) {
    try { return JSON.parse(await readFile(CACHE_PATH, "utf8")); } catch {}
  }
  console.log(`Downloading pinned LoCoMo dataset from ${LOCOMO_SOURCE}`);
  const response = await fetch(LOCOMO_SOURCE, { signal: AbortSignal.timeout(60_000) });
  if (!response.ok) throw new Error(`LoCoMo download failed: HTTP ${response.status}`);
  const parsed = JSON.parse(await response.text());
  await writeFile(CACHE_PATH, `${JSON.stringify(parsed)}\n`, "utf8");
  return parsed;
}

function splitEvidenceIds(evidence) {
  return (Array.isArray(evidence) ? evidence : [])
    .flatMap(value => String(value).split(";"))
    .map(x => x.trim()).filter(Boolean);
}

function buildTurnIndex(conversation) {
  const map = new Map();
  const sessionKeys = Object.keys(conversation)
    .filter(key => /^session_\d+$/.test(key))
    .sort((a, b) => Number(a.split("_")[1]) - Number(b.split("_")[1]));
  let order = 0;
  for (const sessionKey of sessionKeys) {
    const sessionDate = conversation[`${sessionKey}_date_time`] ?? null;
    for (const turn of conversation[sessionKey] ?? []) {
      if (!turn?.dia_id) continue;
      map.set(String(turn.dia_id), {
        dia_id: String(turn.dia_id),
        speaker: String(turn.speaker ?? "Unknown"),
        text: String(turn.text ?? "").trim(),
        blip_caption: turn.blip_caption == null ? null : String(turn.blip_caption).trim(),
        session: sessionKey,
        session_date: sessionDate,
        order: order++,
      });
    }
  }
  return map;
}

function evidenceForQa(sample, qa) {
  const index = buildTurnIndex(sample.conversation ?? {});
  const ids = splitEvidenceIds(qa.evidence);
  const turns = ids.map(id => index.get(id)).filter(Boolean).sort((a, b) => a.order - b.order);
  return { ids, turns, complete: ids.length > 0 && turns.length === ids.length };
}

function selectQuestions(dataset) {
  const selected = [];
  for (const category of categoryIds) {
    const picked = [];
    const used = new Set();
    // First pass: one eligible QA from each conversation for broad coverage.
    for (let sampleIndex = 0; sampleIndex < dataset.length && picked.length < perCategory; sampleIndex++) {
      const sample = dataset[sampleIndex];
      for (let qaIndex = 0; qaIndex < (sample.qa ?? []).length; qaIndex++) {
        const key = `${sampleIndex}:${qaIndex}`;
        const qa = sample.qa[qaIndex];
        if (used.has(key) || Number(qa.category) !== category) continue;
        const evidence = evidenceForQa(sample, qa);
        if (!evidence.complete) continue;
        picked.push({
          id: `${sample.sample_id ?? `sample${sampleIndex + 1}`}:q${qaIndex + 1}`,
          sample, qa, evidence,
        });
        used.add(key);
        break;
      }
    }
    // Fill remaining quota deterministically.
    if (picked.length < perCategory) {
      outer: for (let sampleIndex = 0; sampleIndex < dataset.length; sampleIndex++) {
        const sample = dataset[sampleIndex];
        for (let qaIndex = 0; qaIndex < (sample.qa ?? []).length; qaIndex++) {
          if (picked.length >= perCategory) break outer;
          const key = `${sampleIndex}:${qaIndex}`;
          const qa = sample.qa[qaIndex];
          if (used.has(key) || Number(qa.category) !== category) continue;
          const evidence = evidenceForQa(sample, qa);
          if (!evidence.complete) continue;
          picked.push({
            id: `${sample.sample_id ?? `sample${sampleIndex + 1}`}:q${qaIndex + 1}`,
            sample, qa, evidence,
          });
          used.add(key);
        }
      }
    }
    selected.push(...picked);
  }
  return selected;
}

function formatEvidence(turns) {
  return turns.map(turn => {
    const date = turn.session_date ? ` | session timestamp: ${turn.session_date}` : "";
    const image = turn.blip_caption ? ` [image: ${turn.blip_caption}]` : "";
    return `[${turn.dia_id} | ${turn.session}${date}] ${turn.speaker}: ${turn.text}${image}`;
  }).join("\n");
}

function requestGenerate(body) {
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
        resolve({
          raw: raw.trim(),
          metrics: {
            ttft_ms: firstRaw == null ? null : Math.round(firstRaw - started),
            total_latency_ms: Math.round(performance.now() - started),
            prompt_tokens: Number.isFinite(done.prompt_eval_count) ? done.prompt_eval_count : null,
            output_tokens: Number.isFinite(done.eval_count) ? done.eval_count : null,
            tokens_per_s: evalNs && Number.isFinite(done.eval_count)
              ? Number((done.eval_count / (evalNs / 1e9)).toFixed(2)) : null,
          },
        });
      });
    });
    req.on("error", reject);
    req.setTimeout(90_000, () => req.destroy(new Error("timeout")));
    req.end(JSON.stringify(body));
  });
}

function parseStructured(raw, key) {
  let value = String(raw ?? "").replace(/<think>[\s\S]*?<\/think>/gi, "").trim();
  const start = value.indexOf("{");
  const end = value.lastIndexOf("}");
  if (start >= 0 && end > start) value = value.slice(start, end + 1);
  const parsed = JSON.parse(value);
  if (!(key in parsed)) throw new Error(`structured response missing ${key}`);
  return parsed;
}

async function extractMemories(style, evidenceText) {
  const system = style === "minimal" ? MINIMAL_EXTRACTOR : VOICEMEM_EXTRACTOR;
  const generated = await requestGenerate({
    model,
    prompt: fastPrompt(system, `Conversation evidence:\n${evidenceText}`),
    raw: true,
    stream: true,
    keep_alive: -1,
    format: MEMORY_SCHEMA,
    options: {
      num_predict: extractMaxTokens,
      temperature: 0,
      top_k: 80,
      repeat_penalty: 1.05,
      seed,
      stop: ["<|im_end|>", "<|im_start|>"],
    },
  });
  const parsed = parseStructured(generated.raw, "memory");
  if (!Array.isArray(parsed.memory)) throw new Error('extractor response "memory" is not an array');
  const memories = parsed.memory
    .filter(item => item && typeof item === "object" && typeof item.text === "string" && item.text.trim())
    .map(item => item.text.trim());
  return { memories, metrics: generated.metrics, raw: generated.raw };
}

async function answerQuestion(contextLabel, contextText, question) {
  const generated = await requestGenerate({
    model,
    prompt: fastPrompt(
      ANSWER_SYSTEM,
      `${contextLabel}:\n${contextText}\n\nBenchmark question: ${question}\nReturn only the JSON answer object.`,
    ),
    raw: true,
    stream: true,
    keep_alive: -1,
    format: ANSWER_SCHEMA,
    options: {
      num_predict: answerMaxTokens,
      temperature: 0,
      top_k: 80,
      repeat_penalty: 1.05,
      seed,
      stop: ["<|im_end|>", "<|im_start|>"],
    },
  });
  const parsed = parseStructured(generated.raw, "answer");
  if (typeof parsed.answer !== "string") throw new Error('answer field is not a string');
  return { answer: parsed.answer.trim(), metrics: generated.metrics, raw: generated.raw };
}

function normalizeAnswer(value) {
  return String(value ?? "")
    .toLowerCase()
    .replace(/,/g, "")
    .replace(/[^\p{L}\p{N}\s]/gu, "")
    .replace(/\b(a|an|the|and)\b/gu, " ")
    .replace(/\s+/g, " ")
    .trim();
}

function tokenF1(prediction, groundTruth) {
  const pred = normalizeAnswer(prediction).split(" ").filter(Boolean);
  const gold = normalizeAnswer(groundTruth).split(" ").filter(Boolean);
  if (!pred.length || !gold.length) return pred.length === gold.length ? 1 : 0;
  const counts = new Map();
  for (const token of gold) counts.set(token, (counts.get(token) ?? 0) + 1);
  let common = 0;
  for (const token of pred) {
    const n = counts.get(token) ?? 0;
    if (n > 0) { common++; counts.set(token, n - 1); }
  }
  if (!common) return 0;
  const precision = common / pred.length;
  const recall = common / gold.length;
  return 2 * precision * recall / (precision + recall);
}

function scoreAnswer(prediction, answer, category) {
  if (Number(category) === 1) {
    const predictions = String(prediction).split(",").map(x => x.trim()).filter(Boolean);
    const golds = String(answer).split(",").map(x => x.trim()).filter(Boolean);
    if (!golds.length) return 0;
    return average(golds.map(gold => Math.max(0, ...predictions.map(pred => tokenF1(pred, gold))))) ?? 0;
  }
  return tokenF1(prediction, String(answer));
}

function aggregateLane(rows, lane) {
  const usable = rows.filter(row => row.lanes?.[lane]?.score != null);
  const errors = rows.filter(row => row.lanes?.[lane]?.error);
  const answerLat = usable.map(row => row.lanes[lane].answer_metrics?.total_latency_ms);
  const extractLat = usable.map(row => row.lanes[lane].extract_metrics?.total_latency_ms);
  const categories = {};
  for (const category of categoryIds) {
    const subset = usable.filter(row => Number(row.category) === category);
    categories[CATEGORY_NAMES.get(category) ?? String(category)] = {
      cases: subset.length,
      f1: average(subset.map(row => row.lanes[lane].score)),
    };
  }
  return {
    cases: usable.length,
    errors: errors.length,
    f1: average(usable.map(row => row.lanes[lane].score)),
    by_category: categories,
    answer_latency_ms: { p50: percentile(answerLat, 50), p95: percentile(answerLat, 95), mean: average(answerLat) },
    extract_latency_ms: { p50: percentile(extractLat, 50), p95: percentile(extractLat, 95), mean: average(extractLat) },
    mean_memory_count: average(usable.map(row => row.lanes[lane].memories?.length)),
  };
}

const dataset = await fetchDataset();
if (!Array.isArray(dataset)) throw new Error("LoCoMo dataset root must be an array");
const selected = selectQuestions(dataset);
if (!selected.length) throw new Error("No eligible LoCoMo questions selected");

console.log(`\nLoCoMo Oracle-Gap v2: model=${model} endpoint=${endpoint}`);
console.log(`categories=${categoryIds.map(id => `${id}:${CATEGORY_NAMES.get(id) ?? id}`).join(", ")} per_category=${perCategory} selected=${selected.length}`);
console.log(`lanes=${[...lanes].join(",")}`);
console.log("QA answers are schema-enforced; temporal answers must resolve relative dates from evidence timestamps.");
console.log("Scoring is LoCoMo-style lexical F1 normalization implemented in JS without Porter stemming; use for within-run comparison, not official leaderboard reporting.\n");

const rows = [];
for (let i = 0; i < selected.length; i++) {
  const item = selected[i];
  const evidenceText = formatEvidence(item.evidence.turns);
  const row = {
    id: item.id,
    sample_id: item.sample.sample_id ?? null,
    category: Number(item.qa.category),
    category_name: CATEGORY_NAMES.get(Number(item.qa.category)) ?? String(item.qa.category),
    question: String(item.qa.question),
    answer: String(item.qa.answer),
    evidence_ids: item.evidence.ids,
    evidence: evidenceText,
    lanes: {},
  };

  console.log(`[${i + 1}/${selected.length}] ${row.category_name} ${row.id}: ${row.question}`);

  if (lanes.has("oracle")) {
    try {
      const answered = await answerQuestion("Gold evidence", evidenceText, row.question);
      row.lanes.oracle = {
        answer: answered.answer,
        answer_raw: answered.raw,
        score: scoreAnswer(answered.answer, row.answer, row.category),
        answer_metrics: answered.metrics,
      };
      console.log(`  oracle    F1=${(row.lanes.oracle.score * 100).toFixed(1)}% answer=${JSON.stringify(answered.answer)}`);
    } catch (error) {
      row.lanes.oracle = { error: error.message, score: null };
      console.log(`  oracle    ERR ${error.message}`);
    }
  }

  for (const style of ["minimal", "voicemem"]) {
    if (!lanes.has(style)) continue;
    try {
      const extracted = await extractMemories(style, evidenceText);
      const memoryText = extracted.memories.length
        ? extracted.memories.map((text, index) => `${index + 1}. ${text}`).join("\n")
        : "(no memories extracted)";
      const answered = await answerQuestion("Extracted memories", memoryText, row.question);
      row.lanes[style] = {
        memories: extracted.memories,
        extraction_raw: extracted.raw,
        answer: answered.answer,
        answer_raw: answered.raw,
        score: scoreAnswer(answered.answer, row.answer, row.category),
        extract_metrics: extracted.metrics,
        answer_metrics: answered.metrics,
      };
      console.log(`  ${style.padEnd(9)} F1=${(row.lanes[style].score * 100).toFixed(1)}% mem=${extracted.memories.length} answer=${JSON.stringify(answered.answer)}`);
    } catch (error) {
      row.lanes[style] = { error: error.message, score: null };
      console.log(`  ${style.padEnd(9)} ERR ${error.message}`);
    }
  }
  rows.push(row);
}

const summary = {};
for (const lane of ["oracle", "minimal", "voicemem"]) {
  if (lanes.has(lane)) summary[lane] = aggregateLane(rows, lane);
}
if (summary.oracle) {
  for (const lane of ["minimal", "voicemem"]) {
    if (!summary[lane]) continue;
    summary[lane].compression_gap_vs_oracle = summary.oracle.f1 == null || summary[lane].f1 == null
      ? null : summary.oracle.f1 - summary[lane].f1;
  }
}

await mkdir(RESULTS_DIR, { recursive: true });
const stamp = new Date().toISOString().replace(/[:.]/g, "-");
const modelSlug = model.replace(/[^a-z0-9._-]+/gi, "-").replace(/^-+|-+$/g, "").slice(0, 80) || "model";
const outputPath = path.join(RESULTS_DIR, `${stamp}-locomo-oracle-gap-v2-${modelSlug}.json`);
const report = {
  generated_at: new Date().toISOString(),
  benchmark: "locomo-oracle-gap-v2",
  dataset_source: LOCOMO_SOURCE,
  model,
  endpoint,
  seed,
  categories: categoryIds.map(id => ({ id, name: CATEGORY_NAMES.get(id) ?? String(id) })),
  per_category: perCategory,
  scoring_note: "LoCoMo-style lexical F1 normalization in JS without Porter stemming; intended for within-run comparison, not official leaderboard reporting.",
  summary,
  rows,
};
await writeFile(outputPath, `${JSON.stringify(report, null, 2)}\n`, "utf8");

console.log("\n=== LOCOMO ORACLE-GAP v2 SUMMARY ===");
for (const lane of ["oracle", "minimal", "voicemem"]) {
  const s = summary[lane];
  if (!s) continue;
  const gap = s.compression_gap_vs_oracle == null ? "" : ` gap=${(s.compression_gap_vs_oracle * 100).toFixed(1)}pp`;
  const cats = Object.entries(s.by_category)
    .map(([name, value]) => `${name}=${value.f1 == null ? "n/a" : `${(value.f1 * 100).toFixed(1)}%`}`)
    .join(" ");
  console.log(`${lane.padEnd(9)} F1=${s.f1 == null ? "n/a" : `${(s.f1 * 100).toFixed(1)}%`}${gap} errors=${s.errors} ${cats} answer_p50=${s.answer_latency_ms.p50}ms extract_p50=${s.extract_latency_ms.p50 ?? "-"}ms`);
}
console.log(`\nSaved ${outputPath}`);
