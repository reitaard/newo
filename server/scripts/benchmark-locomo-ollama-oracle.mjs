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
const baseUrl = String(process.env.LOCOMO_OLLAMA_BASE_URL
  ?? process.env.ASSISTANT_BASE_URL
  ?? "http://100.110.136.15:11435").replace(/\/+$/, "");
const endpoint = process.env.LOCOMO_OLLAMA_CHAT_URL ?? `${baseUrl}/api/chat`;
const model = process.env.LOCOMO_OLLAMA_MODEL ?? process.env.LOCOMO_MODEL ?? "";
const perCategory = Math.max(1, Number(process.env.LOCOMO_PER_CATEGORY ?? 10));
const answerMaxTokens = Number(process.env.LOCOMO_ANSWER_MAX_TOKENS ?? 48);
const seed = Number(process.env.LOCOMO_SEED ?? 42);
const categoryIds = String(process.env.LOCOMO_CATEGORIES ?? "4,1,2")
  .split(",").map(x => Number(x.trim())).filter(Number.isFinite);

if (!model) {
  throw new Error("Set LOCOMO_OLLAMA_MODEL to the exact installed Ollama model tag.");
}

const CATEGORY_NAMES = new Map([
  [4, "single_hop"],
  [1, "multi_hop"],
  [2, "temporal"],
  [3, "open_domain"],
  [5, "adversarial"],
]);

const ANSWER_SCHEMA = {
  type: "object",
  additionalProperties: false,
  properties: { answer: { type: "string" } },
  required: ["answer"],
};

const ANSWER_SYSTEM = `You answer a benchmark question from supplied evidence. Return exactly one JSON object with one field: "answer". The answer value must contain only the shortest answer that fully answers the question—never repeat the evidence, question, labels, or instructions. Use only supplied evidence. Resolve relative time expressions such as yesterday/tomorrow using the timestamp attached to that evidence turn. For a list answer, use a concise comma-separated list. Preserve names, dates and numbers. If evidence is insufficient, answer exactly "Not mentioned".`;

function average(values) {
  const usable = values.filter(Number.isFinite);
  return usable.length ? usable.reduce((a, b) => a + b, 0) / usable.length : null;
}

function percentile(values, p) {
  const usable = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!usable.length) return null;
  const i = Math.min(usable.length - 1, Math.max(0, Math.ceil((p / 100) * usable.length) - 1));
  return usable[i];
}

async function fetchDataset() {
  const override = process.env.LOCOMO_DATASET;
  if (override) return JSON.parse(await readFile(path.resolve(override), "utf8"));
  await mkdir(CACHE_DIR, { recursive: true });
  try { return JSON.parse(await readFile(CACHE_PATH, "utf8")); } catch {}
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
    for (let sampleIndex = 0; sampleIndex < dataset.length && picked.length < perCategory; sampleIndex++) {
      const sample = dataset[sampleIndex];
      for (let qaIndex = 0; qaIndex < (sample.qa ?? []).length; qaIndex++) {
        const key = `${sampleIndex}:${qaIndex}`;
        const qa = sample.qa[qaIndex];
        if (used.has(key) || Number(qa.category) !== category) continue;
        const evidence = evidenceForQa(sample, qa);
        if (!evidence.complete) continue;
        picked.push({ id: `${sample.sample_id ?? `sample${sampleIndex + 1}`}:q${qaIndex + 1}`, sample, qa, evidence });
        used.add(key);
        break;
      }
    }
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
          picked.push({ id: `${sample.sample_id ?? `sample${sampleIndex + 1}`}:q${qaIndex + 1}`, sample, qa, evidence });
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

function parseAnswer(content) {
  let cleaned = String(content ?? "")
    .replace(/<think>[\s\S]*?<\/think>/gi, "")
    .replace(/^```(?:json)?\s*/i, "")
    .replace(/\s*```$/i, "")
    .trim();
  const start = cleaned.indexOf("{");
  const end = cleaned.lastIndexOf("}");
  if (start >= 0 && end > start) cleaned = cleaned.slice(start, end + 1);
  const parsed = JSON.parse(cleaned);
  if (typeof parsed.answer !== "string") throw new Error('structured response missing string "answer"');
  return parsed.answer.trim();
}

async function answerQuestion(evidenceText, question) {
  const started = performance.now();
  const response = await fetch(endpoint, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      model,
      messages: [
        { role: "system", content: ANSWER_SYSTEM },
        { role: "user", content: `Gold evidence:\n${evidenceText}\n\nBenchmark question: ${question}\nReturn only the JSON answer object.` },
      ],
      stream: false,
      keep_alive: -1,
      format: ANSWER_SCHEMA,
      options: {
        temperature: 0,
        seed,
        num_predict: answerMaxTokens,
      },
    }),
    signal: AbortSignal.timeout(90_000),
  });
  const bodyText = await response.text();
  if (!response.ok) throw new Error(`HTTP ${response.status}: ${bodyText.slice(0, 300)}`);
  const payload = JSON.parse(bodyText);
  const content = payload?.message?.content;
  if (typeof content !== "string") throw new Error("Ollama chat response missing message.content");
  const answer = parseAnswer(content);
  const evalNs = Number(payload.eval_duration ?? 0);
  return {
    answer,
    raw: content,
    metrics: {
      total_latency_ms: Math.round(performance.now() - started),
      prompt_tokens: Number.isFinite(payload.prompt_eval_count) ? payload.prompt_eval_count : null,
      output_tokens: Number.isFinite(payload.eval_count) ? payload.eval_count : null,
      tokens_per_s: evalNs && Number.isFinite(payload.eval_count)
        ? Number((payload.eval_count / (evalNs / 1e9)).toFixed(2)) : null,
    },
  };
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

const dataset = await fetchDataset();
if (!Array.isArray(dataset)) throw new Error("LoCoMo dataset root must be an array");
const selected = selectQuestions(dataset);
if (!selected.length) throw new Error("No eligible LoCoMo questions selected");

console.log(`\nLoCoMo generic Ollama oracle: model=${model} endpoint=${endpoint}`);
console.log(`categories=${categoryIds.map(id => `${id}:${CATEGORY_NAMES.get(id) ?? id}`).join(", ")} per_category=${perCategory} selected=${selected.length}`);
console.log("Uses Ollama /api/chat + the model's native template; suitable for Gemma and other non-LFM Ollama models.\n");

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
  };
  console.log(`[${i + 1}/${selected.length}] ${row.category_name} ${row.id}: ${row.question}`);
  try {
    const answered = await answerQuestion(evidenceText, row.question);
    row.prediction = answered.answer;
    row.raw = answered.raw;
    row.score = scoreAnswer(answered.answer, row.answer, row.category);
    row.metrics = answered.metrics;
    console.log(`  ollama    F1=${(row.score * 100).toFixed(1)}% answer=${JSON.stringify(row.prediction)} latency=${row.metrics.total_latency_ms}ms t/s=${row.metrics.tokens_per_s ?? "n/a"}`);
  } catch (error) {
    row.error = error.message;
    row.score = null;
    console.log(`  ollama    ERR ${error.message}`);
  }
  rows.push(row);
}

const usable = rows.filter(row => row.score != null);
const byCategory = {};
for (const category of categoryIds) {
  const subset = usable.filter(row => row.category === category);
  byCategory[CATEGORY_NAMES.get(category) ?? String(category)] = {
    cases: subset.length,
    f1: average(subset.map(row => row.score)),
  };
}
const summary = {
  cases: usable.length,
  errors: rows.filter(row => row.error).length,
  f1: average(usable.map(row => row.score)),
  by_category: byCategory,
  answer_latency_ms: {
    p50: percentile(usable.map(row => row.metrics?.total_latency_ms), 50),
    p95: percentile(usable.map(row => row.metrics?.total_latency_ms), 95),
    mean: average(usable.map(row => row.metrics?.total_latency_ms)),
  },
  mean_tokens_per_s: average(usable.map(row => row.metrics?.tokens_per_s)),
};

await mkdir(RESULTS_DIR, { recursive: true });
const stamp = new Date().toISOString().replace(/[:.]/g, "-");
const modelSlug = model.replace(/[^a-z0-9._-]+/gi, "-").replace(/^-+|-+$/g, "").slice(0, 80) || "model";
const outputPath = path.join(RESULTS_DIR, `${stamp}-locomo-ollama-oracle-${modelSlug}.json`);
await writeFile(outputPath, `${JSON.stringify({
  generated_at: new Date().toISOString(),
  benchmark: "locomo-generic-ollama-oracle",
  model,
  endpoint,
  per_category: perCategory,
  summary,
  rows,
}, null, 2)}\n`, "utf8");

console.log("\n=== LOCOMO GENERIC OLLAMA ORACLE SUMMARY ===");
const cats = Object.entries(summary.by_category)
  .map(([name, value]) => `${name}=${value.f1 == null ? "n/a" : `${(value.f1 * 100).toFixed(1)}%`}`)
  .join(" ");
console.log(`ollama    F1=${summary.f1 == null ? "n/a" : `${(summary.f1 * 100).toFixed(1)}%`} errors=${summary.errors} ${cats} answer_p50=${summary.answer_latency_ms.p50}ms p95=${summary.answer_latency_ms.p95}ms mean_tps=${summary.mean_tokens_per_s == null ? "n/a" : summary.mean_tokens_per_s.toFixed(1)}`);
console.log(`Saved ${outputPath}`);
