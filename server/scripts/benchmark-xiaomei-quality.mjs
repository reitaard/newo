import { execFileSync } from "node:child_process";
import { promises as fs } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SERVER_DIR = path.resolve(__dirname, "..");
const FIXTURE_PATH = path.join(SERVER_DIR, "benchmarks", "xiaomei", "quality-v1.json");
const CANDIDATES_PATH = path.join(SERVER_DIR, "benchmarks", "xiaomei", "quality-candidates.json");
const RESULTS_DIR = path.join(SERVER_DIR, "benchmarks", "xiaomei", "results");

const CONTEXT_SIZE = 4096;
const REPEATS = Math.max(1, Number.parseInt(process.env.XIAOMEI_QUALITY_REPEATS || "1", 10) || 1);
const THINK = !/^(0|false|off|no)$/i.test(String(process.env.XIAOMEI_QUALITY_THINK || "true"));
const NUM_PREDICT = Math.max(64, Number.parseInt(process.env.XIAOMEI_QUALITY_NUM_PREDICT || (THINK ? "768" : "256"), 10) || (THINK ? 768 : 256));
const REQUEST_TIMEOUT_MS = Math.max(3_000, Number.parseInt(process.env.XIAOMEI_QUALITY_TIMEOUT_MS || "15000", 10) || 15_000);
const TEMPERATURE = Number.isFinite(Number(process.env.XIAOMEI_QUALITY_TEMPERATURE)) ? Number(process.env.XIAOMEI_QUALITY_TEMPERATURE) : 0.2;
const TOP_P = Number.isFinite(Number(process.env.XIAOMEI_QUALITY_TOP_P)) ? Number(process.env.XIAOMEI_QUALITY_TOP_P) : 0.9;
const TOP_K = Number.isFinite(Number(process.env.XIAOMEI_QUALITY_TOP_K)) ? Number(process.env.XIAOMEI_QUALITY_TOP_K) : 20;

const SYSTEM_PROMPT = [
  "You are Xiaomei, a bilingual English-Chinese voice assistant and language tutor.",
  "The request has already been routed to you; do not classify it or output routing JSON.",
  "Answer the user directly in the language that best fits the request.",
  "Prioritize accurate Mandarin, natural usage, preserved meaning, and practical explanations.",
  "For Mandarin pronunciation, use standard Hanyu Pinyin with tone marks.",
  "Keep the final spoken answer concise and natural, normally two to six sentences.",
  "Do not use markdown, tables, headings, or hidden reasoning in the final answer.",
].join(" ");

function median(values) {
  const sorted = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!sorted.length) return null;
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

function round(value, digits = 1) {
  return Number.isFinite(value) ? Number(value.toFixed(digits)) : null;
}

function gitValue(args) {
  try {
    return execFileSync("git", args, { cwd: path.resolve(SERVER_DIR, ".."), encoding: "utf8" }).trim();
  } catch {
    return null;
  }
}

function normalize(value) {
  return String(value || "").normalize("NFKC").toLowerCase();
}

function safeModelName(value) {
  return String(value || "unknown-model").replace(/[^a-zA-Z0-9._-]+/g, "_").slice(0, 120);
}

function baseUrlFromEndpoint(value) {
  return String(value || "").trim().replace(/\/api\/chat\/?$/i, "").replace(/\/$/, "");
}

async function readJson(file) {
  return JSON.parse(await fs.readFile(file, "utf8"));
}

async function fetchJson(url, init = {}, timeoutMs = REQUEST_TIMEOUT_MS) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, { ...init, signal: controller.signal });
    const text = await response.text();
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}: ${text.slice(0, 400)}`);
    return text ? JSON.parse(text) : {};
  } finally {
    clearTimeout(timer);
  }
}

async function unloadModel(baseUrl, model) {
  try {
    await fetchJson(`${baseUrl}/api/generate`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ model, prompt: "", stream: false, keep_alive: 0 }),
    }, 10_000);
    return true;
  } catch {
    return false;
  }
}

async function preloadModel(baseUrl, model) {
  try {
    await fetchJson(`${baseUrl}/api/generate`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ model, prompt: "", stream: false, keep_alive: -1 }),
    }, 20_000);
    return true;
  } catch {
    return false;
  }
}

function scoreResponse(caseDef, text) {
  const source = normalize(text);
  const checks = caseDef.checks || {};
  const details = [];
  let passed = 0;
  let total = 0;

  for (const group of checks.must_include_any || []) {
    total += 1;
    const ok = Array.isArray(group) && group.some((term) => source.includes(normalize(term)));
    if (ok) passed += 1;
    details.push({ type: "must_include_any", values: group, pass: ok });
  }

  for (const term of checks.must_not_include || []) {
    total += 1;
    const ok = !source.includes(normalize(term));
    if (ok) passed += 1;
    details.push({ type: "must_not_include", value: term, pass: ok });
  }

  if (Number.isFinite(checks.max_chars)) {
    total += 1;
    const ok = String(text || "").length <= checks.max_chars;
    if (ok) passed += 1;
    details.push({ type: "max_chars", value: checks.max_chars, actual: String(text || "").length, pass: ok });
  }

  return {
    passed,
    total,
    pass: total > 0 ? passed === total : null,
    score_pct: total > 0 ? round((passed / total) * 100) : null,
    details,
  };
}

async function streamAnswer({ baseUrl, model, prompt }) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  const startedNs = process.hrtime.bigint();
  let firstContentNs = null;
  let text = "";
  let thinking = "";
  let finalChunk = null;

  try {
    const response = await fetch(`${baseUrl}/api/chat`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      signal: controller.signal,
      body: JSON.stringify({
        model,
        messages: [
          { role: "system", content: SYSTEM_PROMPT },
          { role: "user", content: prompt },
        ],
        stream: true,
        think: THINK,
        keep_alive: -1,
        options: {
          num_ctx: CONTEXT_SIZE,
          temperature: TEMPERATURE,
          top_p: TOP_P,
          top_k: TOP_K,
          num_predict: NUM_PREDICT,
        },
      }),
    });

    if (!response.ok || !response.body) {
      const body = await response.text();
      throw new Error(`${response.status} ${response.statusText}: ${body.slice(0, 400)}`);
    }

    const decoder = new TextDecoder();
    let buffer = "";
    for await (const chunk of response.body) {
      buffer += decoder.decode(chunk, { stream: true });
      while (true) {
        const newline = buffer.indexOf("\n");
        if (newline < 0) break;
        const line = buffer.slice(0, newline).trim();
        buffer = buffer.slice(newline + 1);
        if (!line) continue;
        const item = JSON.parse(line);
        if (item.message?.thinking) thinking += item.message.thinking;
        if (item.message?.content) {
          if (firstContentNs == null) firstContentNs = process.hrtime.bigint();
          text += item.message.content;
        }
        if (item.done) finalChunk = item;
      }
    }
    buffer += decoder.decode();
    if (buffer.trim()) {
      const item = JSON.parse(buffer.trim());
      if (item.message?.thinking) thinking += item.message.thinking;
      if (item.message?.content) {
        if (firstContentNs == null) firstContentNs = process.hrtime.bigint();
        text += item.message.content;
      }
      if (item.done) finalChunk = item;
    }
  } catch (error) {
    if (error?.name === "AbortError") throw new Error(`timeout after ${REQUEST_TIMEOUT_MS}ms`);
    throw error;
  } finally {
    clearTimeout(timer);
  }

  const endedNs = process.hrtime.bigint();
  const evalDurationSec = Number(finalChunk?.eval_duration || 0) / 1e9;
  return {
    text: text.trim(),
    thinking: thinking.trim(),
    content_chars: text.trim().length,
    thinking_chars: thinking.trim().length,
    first_content_ms: firstContentNs == null ? null : round(Number(firstContentNs - startedNs) / 1e6),
    total_ms: round(Number(endedNs - startedNs) / 1e6),
    eval_count: finalChunk?.eval_count ?? null,
    decode_tps: evalDurationSec > 0 ? round((finalChunk?.eval_count || 0) / evalDurationSec) : null,
    done_reason: finalChunk?.done_reason ?? null,
  };
}

async function runCandidate({ candidate, fixture, baseUrl }) {
  await unloadModel(baseUrl, candidate.model);
  const preloaded = await preloadModel(baseUrl, candidate.model);
  const results = [];

  for (const caseDef of fixture.cases) {
    process.stdout.write(`[xiaomei-quality] ${candidate.label} / ${caseDef.id} ... `);
    const attempts = [];
    for (let attempt = 1; attempt <= REPEATS; attempt += 1) {
      try {
        const measurement = await streamAnswer({ baseUrl, model: candidate.model, prompt: caseDef.prompt });
        const anchors = scoreResponse(caseDef, measurement.text);
        attempts.push({ attempt, ...measurement, anchors });
      } catch (error) {
        attempts.push({ attempt, error: error.message, anchors: { passed: 0, total: 0, pass: false, score_pct: null, details: [] } });
      }
    }
    const first = attempts[0];
    const status = first.error ? `ERROR ${first.error}` : `${first.anchors.pass ? "PASS" : "REVIEW"} ${first.total_ms ?? "?"}ms`;
    process.stdout.write(`${status}\n`);
    results.push({ id: caseDef.id, category: caseDef.category, prompt: caseDef.prompt, attempts });
  }

  const attempts = results.flatMap((result) => result.attempts);
  const successful = attempts.filter((attempt) => !attempt.error);
  const anchorPassed = successful.reduce((sum, attempt) => sum + (attempt.anchors?.passed || 0), 0);
  const anchorTotal = successful.reduce((sum, attempt) => sum + (attempt.anchors?.total || 0), 0);
  const casesWithChecks = successful.filter((attempt) => attempt.anchors?.total > 0);
  const truncations = successful.filter((attempt) => attempt.done_reason === "length").length;

  return {
    label: candidate.label,
    model: candidate.model,
    preloaded,
    summary: {
      attempts: attempts.length,
      errors: attempts.length - successful.length,
      anchor_score_pct: anchorTotal ? round((anchorPassed / anchorTotal) * 100) : null,
      full_anchor_pass_pct: casesWithChecks.length ? round((casesWithChecks.filter((attempt) => attempt.anchors.pass).length / casesWithChecks.length) * 100) : null,
      median_first_content_ms: round(median(successful.map((attempt) => attempt.first_content_ms))),
      median_total_ms: round(median(successful.map((attempt) => attempt.total_ms))),
      median_thinking_chars: round(median(successful.map((attempt) => attempt.thinking_chars))),
      median_decode_tps: round(median(successful.map((attempt) => attempt.decode_tps))),
      truncation_pct: successful.length ? round((truncations / successful.length) * 100) : null,
    },
    results,
  };
}

function sanitizeFence(text) {
  return String(text || "").replace(/```/g, "''' ");
}

function markdownReport(report) {
  const lines = [];
  lines.push("# Xiaomei Response Quality Shootout");
  lines.push("");
  lines.push(`- Generated: ${report.generated_at}`);
  lines.push(`- Branch: ${report.git.branch || "unknown"}`);
  lines.push(`- Commit: ${report.git.commit || "unknown"}`);
  lines.push(`- Endpoint: \`${report.endpoint}\``);
  lines.push(`- Context: ${report.config.context_size}`);
  lines.push(`- THINK: ${report.config.think}`);
  lines.push(`- Output budget: ${report.config.num_predict} tokens`);
  lines.push(`- Hard timeout: ${report.config.timeout_ms} ms`);
  lines.push(`- Repeats: ${report.config.repeats}`);
  lines.push("");
  lines.push("Automated anchors catch concrete mistakes such as missing ‘tomorrow’, wrong pinyin, or lost negation. They are not a substitute for reading the responses.");
  lines.push("");
  lines.push("## Comparison");
  lines.push("");
  lines.push("| Candidate | Anchor score | Full anchor pass | First ms | Total ms | Thinking chars | Decode t/s | Truncated | Errors |");
  lines.push("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |");
  for (const candidate of report.candidates) {
    const s = candidate.summary;
    lines.push(`| ${candidate.label} | ${s.anchor_score_pct ?? "n/a"}% | ${s.full_anchor_pass_pct ?? "n/a"}% | ${s.median_first_content_ms ?? "n/a"} | ${s.median_total_ms ?? "n/a"} | ${s.median_thinking_chars ?? "n/a"} | ${s.median_decode_tps ?? "n/a"} | ${s.truncation_pct ?? "n/a"}% | ${s.errors} |`);
  }

  for (const candidate of report.candidates) {
    lines.push("");
    lines.push(`## ${candidate.label}`);
    lines.push("");
    for (const result of candidate.results) {
      const attempt = result.attempts[0];
      lines.push(`### ${result.id}`);
      lines.push("");
      lines.push(`Prompt: ${result.prompt}`);
      lines.push("");
      if (attempt.error) {
        lines.push(`ERROR: ${attempt.error}`);
      } else {
        lines.push(`Anchors: ${attempt.anchors.passed}/${attempt.anchors.total} (${attempt.anchors.score_pct ?? "n/a"}%) · first ${attempt.first_content_ms ?? "n/a"} ms · total ${attempt.total_ms ?? "n/a"} ms · thinking ${attempt.thinking_chars} chars · done ${attempt.done_reason || "n/a"}`);
        lines.push("");
        lines.push("```text");
        lines.push(sanitizeFence(attempt.text));
        lines.push("```");
      }
      lines.push("");
    }
  }

  lines.push("## Human review rule");
  lines.push("");
  lines.push("Choose the quality model only after checking Mandarin correctness, naturalness, teaching usefulness, preservation of dates/numbers/negation, and whether latency is acceptable for spoken interaction. Do not select from the anchor percentage alone.");
  lines.push("");
  return `${lines.join("\n")}\n`;
}

async function main() {
  const fixture = await readJson(FIXTURE_PATH);
  const config = await readJson(CANDIDATES_PATH);
  if (!Array.isArray(fixture.cases) || fixture.cases.length === 0) throw new Error("quality fixture contains no cases");
  if (!Array.isArray(config.candidates) || config.candidates.length === 0) throw new Error("quality candidates contains no models");

  const baseUrl = String(process.env.XIAOMEI_QUALITY_BASE_URL || baseUrlFromEndpoint(config.endpoint)).replace(/\/$/, "");
  const requestedModel = String(process.env.XIAOMEI_QUALITY_MODEL || "").trim();
  const candidates = requestedModel
    ? (config.candidates.some((candidate) => candidate.model === requestedModel)
        ? config.candidates.filter((candidate) => candidate.model === requestedModel)
        : [{ label: requestedModel, model: requestedModel }])
    : config.candidates;

  const report = {
    schema_version: 1,
    benchmark: fixture.name,
    generated_at: new Date().toISOString(),
    git: {
      branch: gitValue(["rev-parse", "--abbrev-ref", "HEAD"]),
      commit: gitValue(["rev-parse", "HEAD"]),
    },
    endpoint: `${baseUrl}/api/chat`,
    config: {
      context_size: CONTEXT_SIZE,
      repeats: REPEATS,
      think: THINK,
      num_predict: NUM_PREDICT,
      timeout_ms: REQUEST_TIMEOUT_MS,
      temperature: TEMPERATURE,
      top_p: TOP_P,
      top_k: TOP_K,
    },
    candidates: [],
  };

  await fs.mkdir(RESULTS_DIR, { recursive: true });
  for (const candidate of candidates) {
    console.log(`\n[xiaomei-quality] === ${candidate.label} / ${candidate.model} ===`);
    report.candidates.push(await runCandidate({ candidate, fixture, baseUrl }));
    await unloadModel(baseUrl, candidate.model);
  }

  const stamp = report.generated_at.replace(/[:.]/g, "-");
  const suffix = THINK ? "think" : "fast";
  const single = candidates.length === 1 ? `__${safeModelName(candidates[0].model)}` : "";
  const stem = `QUALITY-SHOOTOUT__${suffix}${single}__${stamp}`;
  const jsonText = `${JSON.stringify(report, null, 2)}\n`;
  const mdText = markdownReport(report);
  await Promise.all([
    fs.writeFile(path.join(RESULTS_DIR, `${stem}.json`), jsonText),
    fs.writeFile(path.join(RESULTS_DIR, `${stem}.md`), mdText),
    fs.writeFile(path.join(RESULTS_DIR, "QUALITY-SHOOTOUT-LATEST.json"), jsonText),
    fs.writeFile(path.join(RESULTS_DIR, "QUALITY-SHOOTOUT-LATEST.md"), mdText),
  ]);

  console.log("\n[xiaomei-quality] report: benchmarks/xiaomei/results/QUALITY-SHOOTOUT-LATEST.md");
}

main().catch((error) => {
  console.error(`[xiaomei-quality] failed: ${error.stack || error.message}`);
  process.exitCode = 1;
});
