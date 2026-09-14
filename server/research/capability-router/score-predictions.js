import { readFileSync } from "node:fs";
import { resolve } from "node:path";

function readJsonl(path) {
  return readFileSync(path, "utf8").split(/\r?\n/).filter(Boolean).map((line, index) => {
    try { return JSON.parse(line); }
    catch (error) { throw new Error(`${path}:${index + 1}: invalid JSON`, { cause: error }); }
  });
}

function percentile(values, p) {
  const sorted = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!sorted.length) return null;
  return sorted[Math.max(0, Math.ceil(sorted.length * p / 100) - 1)];
}

function normalizePrediction(row) {
  const scores = row?.scores && typeof row.scores === "object" && !Array.isArray(row.scores) ? row.scores : {};
  const ranked = Object.entries(scores)
    .filter(([, value]) => Number.isFinite(value))
    .sort((a, b) => b[1] - a[1])
    .map(([id]) => id);
  const explicit = Array.isArray(row.capabilities) ? row.capabilities.filter((value) => typeof value === "string") : [];
  const capabilities = explicit.length ? explicit : ranked.slice(0, row.abstain ? 0 : 1);
  return {
    ...row,
    scores,
    ranked,
    capabilities,
    primary: row.primary ?? capabilities[0] ?? null,
    abstain: Boolean(row.abstain),
  };
}

function sameSet(a, b) {
  if (a.length !== b.length) return false;
  const left = [...a].sort();
  const right = [...b].sort();
  return left.every((value, index) => value === right[index]);
}

function ratio(numerator, denominator) {
  return denominator ? numerator / denominator : null;
}

function pct(value) { return value == null ? "n/a" : `${(value * 100).toFixed(1)}%`; }
function ms(value) { return value == null ? "n/a" : `${value.toFixed(1)} ms`; }

const corpusPath = resolve(process.argv[2] || new URL("./corpus.jsonl", import.meta.url).pathname);
const predictionArg = process.argv[3];
if (!predictionArg) {
  console.error("usage: node score-predictions.js [corpus.jsonl] predictions.jsonl");
  process.exit(2);
}
const predictionsPath = resolve(predictionArg);
const corpus = readJsonl(corpusPath);
const predictionRows = readJsonl(predictionsPath).map(normalizePrediction);
const byId = new Map(predictionRows.map((row) => [row.id, row]));

const missing = corpus.filter((row) => !byId.has(row.id));
if (missing.length) throw new Error(`missing predictions for ${missing.length} corpus rows: ${missing.slice(0, 8).map((row) => row.id).join(", ")}`);

let primaryCorrect = 0;
let exactCorrect = 0;
let top2Hits = 0;
let top2Eligible = 0;
let falseTool = 0;
let noToolCount = 0;
let falseWeb = 0;
let noWebCount = 0;
let missedLive = 0;
let liveCount = 0;
let abstainTP = 0, abstainFP = 0, abstainFN = 0;
let specializedExpected = 0, specializedToWeb = 0;
const confusions = new Map();
const latencies = [];

for (const expected of corpus) {
  const predicted = byId.get(expected.id);
  const predictedPrimary = predicted.primary;
  if (predictedPrimary === expected.primary) primaryCorrect += 1;
  if (sameSet(predicted.capabilities, expected.expected_capabilities)) exactCorrect += 1;

  if (expected.primary) {
    top2Eligible += 1;
    const top2 = predicted.ranked.length ? predicted.ranked.slice(0, 2) : predicted.capabilities.slice(0, 2);
    if (top2.includes(expected.primary)) top2Hits += 1;
  }

  const expectsNoTool = expected.primary === "knowledge" || expected.abstain === true;
  if (expectsNoTool) {
    noToolCount += 1;
    if (predicted.capabilities.some((id) => id !== "knowledge")) falseTool += 1;
  }

  const expectsWeb = expected.expected_capabilities.some((id) => id.startsWith("web."));
  if (!expectsWeb) {
    noWebCount += 1;
    if (predicted.capabilities.some((id) => id.startsWith("web."))) falseWeb += 1;
  }

  if (expected.source_need === "live") {
    liveCount += 1;
    if (predicted.source_need && predicted.source_need !== "live") missedLive += 1;
  }

  if (expected.abstain && predicted.abstain) abstainTP += 1;
  if (!expected.abstain && predicted.abstain) abstainFP += 1;
  if (expected.abstain && !predicted.abstain) abstainFN += 1;

  const specialized = expected.expected_capabilities.some((id) => [
    "currency.exchange", "weather.current", "weather.forecast", "sports.score", "sports.schedule", "market.quote",
    "calculator", "unit.convert", "time.current", "time.convert", "device.state", "sensor.read", "camera.inspect",
  ].includes(id));
  if (specialized) {
    specializedExpected += 1;
    if (predictedPrimary?.startsWith("web.")) specializedToWeb += 1;
  }

  if (Number.isFinite(predicted.latency_ms)) latencies.push(predicted.latency_ms);
  if (predictedPrimary !== expected.primary) {
    const key = `${expected.primary ?? "<abstain>"} -> ${predictedPrimary ?? (predicted.abstain ? "<abstain>" : "<none>")}`;
    confusions.set(key, (confusions.get(key) ?? 0) + 1);
  }
}

const abstainPrecision = ratio(abstainTP, abstainTP + abstainFP);
const abstainRecall = ratio(abstainTP, abstainTP + abstainFN);
const metrics = {
  rows: corpus.length,
  primary_accuracy: ratio(primaryCorrect, corpus.length),
  capability_set_exact_match: ratio(exactCorrect, corpus.length),
  top2_primary_recall: ratio(top2Hits, top2Eligible),
  false_tool_rate: ratio(falseTool, noToolCount),
  false_web_rate: ratio(falseWeb, noWebCount),
  missed_live_rate: ratio(missedLive, liveCount),
  abstention_precision: abstainPrecision,
  abstention_recall: abstainRecall,
  specialized_routed_to_web_rate: ratio(specializedToWeb, specializedExpected),
  latency_ms: {
    samples: latencies.length,
    p50: percentile(latencies, 50),
    p95: percentile(latencies, 95),
    p99: percentile(latencies, 99),
  },
};

console.log(JSON.stringify(metrics, null, 2));
console.log("\nReadable summary");
console.log(`primary accuracy                 ${pct(metrics.primary_accuracy)}`);
console.log(`capability-set exact match       ${pct(metrics.capability_set_exact_match)}`);
console.log(`top-2 primary recall             ${pct(metrics.top2_primary_recall)}`);
console.log(`false-tool rate                  ${pct(metrics.false_tool_rate)}`);
console.log(`false-web rate                   ${pct(metrics.false_web_rate)}`);
console.log(`missed-live rate                 ${pct(metrics.missed_live_rate)}`);
console.log(`abstention precision / recall    ${pct(metrics.abstention_precision)} / ${pct(metrics.abstention_recall)}`);
console.log(`specialized -> web error rate    ${pct(metrics.specialized_routed_to_web_rate)}`);
console.log(`latency p50 / p95 / p99          ${ms(metrics.latency_ms.p50)} / ${ms(metrics.latency_ms.p95)} / ${ms(metrics.latency_ms.p99)}`);

const worst = [...confusions.entries()].sort((a, b) => b[1] - a[1]).slice(0, 12);
if (worst.length) {
  console.log("\nTop confusions");
  for (const [key, count] of worst) console.log(`${String(count).padStart(3)}  ${key}`);
}
