import { execFileSync } from "node:child_process";
import { promises as fs } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { createAssistantProfiles, resolveAssistantProfile } from "../src/assistant-profiles.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SERVER_DIR = path.resolve(__dirname, "..");
const FIXTURE_PATH = path.join(SERVER_DIR, "benchmarks", "xiaomei", "controller-v1.json");
const RESULTS_DIR = path.join(SERVER_DIR, "benchmarks", "xiaomei", "results");
const CONTEXT_SIZE = 4096;
const PROFILE_INPUT = process.env.XIAOMEI_BENCH_PROFILE?.trim() || "main";
const REPEATS = Math.max(1, Number.parseInt(process.env.XIAOMEI_BENCH_REPEATS || "1", 10) || 1);
const REQUEST_TIMEOUT_MS = Math.max(5_000, Number.parseInt(process.env.XIAOMEI_BENCH_TIMEOUT_MS || "45000", 10) || 45_000);

const XIAOMEI_CONTROLLER_PROMPT = [
  "You are Xiaomei, a bilingual English-Chinese voice assistant and interpreter controller.",
  "Classify the user's latest intent using exactly one route:",
  "chat, teach, translate_once, interpreter_start, interpreter_translate, interpreter_meta, interpreter_stop.",
  "Use chat for ordinary conversation in English or Chinese.",
  "Use teach when the user asks to learn, explain, pronounce, or understand Chinese or English.",
  "Use translate_once for a one-off translation when continuous interpreter mode is not active.",
  "Use interpreter_start when the user asks you to continuously translate between English and Chinese.",
  "While interpreter mode is active, use interpreter_translate for speech or instructions intended for the other person.",
  "While interpreter mode is active, use interpreter_meta when the user is talking to you about the conversation, asking what something meant, or asking for an explanation rather than asking you to relay it.",
  "Use interpreter_stop when the user asks to leave interpreter mode.",
  "Return exactly one JSON object with keys route and reply. Do not use markdown or code fences.",
  "The reply should be brief and natural. For interpreter_translate, preserve the speaker's meaning and do not add commentary.",
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

function safeModelName(value) {
  return String(value || "unknown-model").replace(/[^a-zA-Z0-9._-]+/g, "_").slice(0, 120);
}

function gitValue(args) {
  try {
    return execFileSync("git", args, { cwd: path.resolve(SERVER_DIR, ".."), encoding: "utf8" }).trim();
  } catch {
    return null;
  }
}

async function readFixture() {
  const fixture = JSON.parse(await fs.readFile(FIXTURE_PATH, "utf8"));
  if (!Array.isArray(fixture.cases) || fixture.cases.length === 0) throw new Error("Xiaomei benchmark fixture contains no cases");
  return fixture;
}

async function fetchJson(url, init = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  try {
    const response = await fetch(url, { ...init, signal: controller.signal });
    const text = await response.text();
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}: ${text.slice(0, 500)}`);
    return text ? JSON.parse(text) : {};
  } finally {
    clearTimeout(timer);
  }
}

async function ollamaVersion(baseUrl) {
  try {
    return await fetchJson(`${baseUrl}/api/version`);
  } catch (error) {
    return { error: error.message };
  }
}

async function ollamaPs(baseUrl) {
  try {
    return await fetchJson(`${baseUrl}/api/ps`);
  } catch (error) {
    return { error: error.message };
  }
}

async function unloadModel(baseUrl, model) {
  try {
    await fetchJson(`${baseUrl}/api/generate`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ model, prompt: "", stream: false, keep_alive: 0 }),
    });
    return true;
  } catch {
    return false;
  }
}

async function streamChat({ baseUrl, model, messages, systemPrompt = XIAOMEI_CONTROLLER_PROMPT }) {
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
        messages: [{ role: "system", content: systemPrompt }, ...messages],
        stream: true,
        think: false,
        keep_alive: -1,
        options: {
          num_ctx: CONTEXT_SIZE,
          temperature: 0.2,
          top_p: 0.9,
          num_predict: 192,
        },
      }),
    });
    if (!response.ok || !response.body) {
      const body = await response.text();
      throw new Error(`${response.status} ${response.statusText}: ${body.slice(0, 500)}`);
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
  } finally {
    clearTimeout(timer);
  }

  const endedNs = process.hrtime.bigint();
  const elapsedMs = Number(endedNs - startedNs) / 1e6;
  const firstContentMs = firstContentNs == null ? null : Number(firstContentNs - startedNs) / 1e6;
  const evalDurationSec = Number(finalChunk?.eval_duration || 0) / 1e9;
  const promptDurationSec = Number(finalChunk?.prompt_eval_duration || 0) / 1e9;

  return {
    text: text.trim(),
    thinking: thinking.trim(),
    first_content_ms: round(firstContentMs),
    total_ms: round(elapsedMs),
    load_ms: round(Number(finalChunk?.load_duration || 0) / 1e6),
    prompt_eval_count: finalChunk?.prompt_eval_count ?? null,
    prompt_eval_tps: promptDurationSec > 0 ? round((finalChunk?.prompt_eval_count || 0) / promptDurationSec) : null,
    eval_count: finalChunk?.eval_count ?? null,
    decode_tps: evalDurationSec > 0 ? round((finalChunk?.eval_count || 0) / evalDurationSec) : null,
    done_reason: finalChunk?.done_reason ?? null,
  };
}

function parseControllerJson(text) {
  const trimmed = String(text || "").trim().replace(/^```(?:json)?\s*/i, "").replace(/\s*```$/i, "");
  const candidates = [trimmed];
  const first = trimmed.indexOf("{");
  const last = trimmed.lastIndexOf("}");
  if (first >= 0 && last > first) candidates.push(trimmed.slice(first, last + 1));
  for (const candidate of candidates) {
    try {
      const parsed = JSON.parse(candidate);
      if (parsed && typeof parsed === "object") return { valid: true, value: parsed };
    } catch {}
  }
  return { valid: false, value: null };
}

function replyCheck(caseDef, reply) {
  const includesAny = caseDef.reply_checks?.includes_any;
  if (!Array.isArray(includesAny) || includesAny.length === 0) return null;
  const normalized = String(reply || "").toLowerCase();
  return includesAny.some((item) => normalized.includes(String(item).toLowerCase()));
}

async function runCase({ caseDef, baseUrl, model }) {
  const attemptResults = [];
  for (let attempt = 1; attempt <= REPEATS; attempt += 1) {
    const timing = await streamChat({ baseUrl, model, messages: caseDef.messages });
    const parsed = parseControllerJson(timing.text);
    const route = String(parsed.value?.route || "").trim().toLowerCase() || null;
    const reply = typeof parsed.value?.reply === "string" ? parsed.value.reply.trim() : "";
    attemptResults.push({
      attempt,
      ...timing,
      json_valid: parsed.valid,
      route,
      reply,
      route_match: route === caseDef.expected_route,
      reply_check: replyCheck(caseDef, reply),
    });
  }
  return {
    id: caseDef.id,
    category: caseDef.category,
    expected_route: caseDef.expected_route,
    messages: caseDef.messages,
    attempts: attemptResults,
  };
}

function summarize(results) {
  const attempts = results.flatMap((result) => result.attempts);
  const routeMatches = attempts.filter((attempt) => attempt.route_match).length;
  const jsonValid = attempts.filter((attempt) => attempt.json_valid).length;
  const checkedReplies = attempts.filter((attempt) => attempt.reply_check != null);
  return {
    cases: results.length,
    attempts: attempts.length,
    route_accuracy_pct: round((routeMatches / Math.max(1, attempts.length)) * 100),
    json_valid_pct: round((jsonValid / Math.max(1, attempts.length)) * 100),
    reply_check_pct: checkedReplies.length ? round((checkedReplies.filter((attempt) => attempt.reply_check).length / checkedReplies.length) * 100) : null,
    median_first_content_ms: round(median(attempts.map((attempt) => attempt.first_content_ms))),
    median_total_ms: round(median(attempts.map((attempt) => attempt.total_ms))),
    median_prompt_eval_tps: round(median(attempts.map((attempt) => attempt.prompt_eval_tps))),
    median_decode_tps: round(median(attempts.map((attempt) => attempt.decode_tps))),
    median_load_ms: round(median(attempts.map((attempt) => attempt.load_ms))),
  };
}

function markdownReport(report) {
  const lines = [];
  lines.push("# Xiaomei Controller Benchmark");
  lines.push("");
  lines.push(`- Generated: ${report.generated_at}`);
  lines.push(`- Branch: ${report.git.branch || "unknown"}`);
  lines.push(`- Commit: ${report.git.commit || "unknown"}`);
  lines.push(`- Profile slot: ${report.target.profile}`);
  lines.push(`- Model: \`${report.target.model}\``);
  lines.push(`- Endpoint: \`${report.target.base_url}\``);
  lines.push(`- Context: ${report.config.context_size}`);
  lines.push(`- Repeats: ${report.config.repeats}`);
  lines.push("");
  lines.push("## Summary");
  lines.push("");
  lines.push("| Metric | Result |");
  lines.push("| --- | ---: |");
  lines.push(`| Route accuracy | ${report.summary.route_accuracy_pct ?? "n/a"}% |`);
  lines.push(`| Valid JSON | ${report.summary.json_valid_pct ?? "n/a"}% |`);
  lines.push(`| Reply checks | ${report.summary.reply_check_pct == null ? "n/a" : `${report.summary.reply_check_pct}%`} |`);
  lines.push(`| Median first content | ${report.summary.median_first_content_ms ?? "n/a"} ms |`);
  lines.push(`| Median total response | ${report.summary.median_total_ms ?? "n/a"} ms |`);
  lines.push(`| Median prefill | ${report.summary.median_prompt_eval_tps ?? "n/a"} tok/s |`);
  lines.push(`| Median decode | ${report.summary.median_decode_tps ?? "n/a"} tok/s |`);
  lines.push("");
  lines.push("## Cold-start probe");
  lines.push("");
  lines.push(`- Unload request succeeded: ${report.cold_probe.unload_succeeded}`);
  lines.push(`- First content: ${report.cold_probe.measurement.first_content_ms ?? "n/a"} ms`);
  lines.push(`- Total: ${report.cold_probe.measurement.total_ms ?? "n/a"} ms`);
  lines.push(`- Ollama load duration: ${report.cold_probe.measurement.load_ms ?? "n/a"} ms`);
  lines.push("");
  lines.push("## Residency");
  lines.push("");
  if (report.residency?.model) {
    lines.push(`- Reported size: ${report.residency.model.size ?? "n/a"}`);
    lines.push(`- Reported VRAM: ${report.residency.model.size_vram ?? "n/a"}`);
    lines.push(`- Expires at: ${report.residency.model.expires_at ?? "n/a"}`);
  } else {
    lines.push("- Target model was not found in `/api/ps` after the run.");
  }
  lines.push("");
  lines.push("## Cases");
  lines.push("");
  lines.push("| Case | Expected | Actual | Pass | First ms | Total ms | Decode t/s |");
  lines.push("| --- | --- | --- | :---: | ---: | ---: | ---: |");
  for (const result of report.results) {
    const first = result.attempts[0];
    lines.push(`| ${result.id} | ${result.expected_route} | ${first.route || "invalid"} | ${first.route_match ? "✓" : "✗"} | ${first.first_content_ms ?? "n/a"} | ${first.total_ms ?? "n/a"} | ${first.decode_tps ?? "n/a"} |`);
  }
  lines.push("");
  lines.push("## Human review");
  lines.push("");
  lines.push("Fill these after reading the raw replies in the JSON report:");
  lines.push("");
  lines.push("- English conversational quality (1-5):");
  lines.push("- Chinese conversational quality (1-5):");
  lines.push("- Code-switch understanding (1-5):");
  lines.push("- Chinese teaching quality (1-5):");
  lines.push("- Interpreter/meta-command judgment (1-5):");
  lines.push("- Voice-response suitability / brevity (1-5):");
  lines.push("- Notes:");
  lines.push("");
  lines.push("## Decision gate");
  lines.push("");
  lines.push("Do not promote a candidate only because it is faster. Compare route accuracy, Chinese quality, warm/cold latency, and residency headroom against the same fixture and 4096 context.");
  lines.push("");
  return `${lines.join("\n")}\n`;
}

async function main() {
  const profiles = createAssistantProfiles();
  const profileId = resolveAssistantProfile(PROFILE_INPUT, profiles);
  if (!profileId) throw new Error(`Unknown assistant profile: ${PROFILE_INPUT}`);
  const profile = profiles[profileId];
  const baseUrl = (process.env.XIAOMEI_BENCH_BASE_URL?.trim() || profile.baseUrl).replace(/\/$/, "");
  const model = process.env.XIAOMEI_BENCH_MODEL?.trim() || profile.model;
  if (!baseUrl || !model) throw new Error("Benchmark target requires both base URL and model ID");
  if (!process.env.XIAOMEI_BENCH_BASE_URL && !process.env.XIAOMEI_BENCH_MODEL && profile.provider !== "ollama_chat") {
    throw new Error(`Profile ${profileId} is ${profile.provider}; Xiaomei benchmark currently requires an Ollama /api/chat target`);
  }

  const fixture = await readFixture();
  const generatedAt = new Date().toISOString();
  const version = await ollamaVersion(baseUrl);
  const beforePs = await ollamaPs(baseUrl);
  const unloadSucceeded = await unloadModel(baseUrl, model);
  const coldMeasurement = await streamChat({
    baseUrl,
    model,
    systemPrompt: "Reply with exactly READY. Do not add anything else.",
    messages: [{ role: "user", content: "READY" }],
  });

  const results = [];
  for (const caseDef of fixture.cases) {
    process.stdout.write(`[xiaomei] ${caseDef.id} ... `);
    try {
      const result = await runCase({ caseDef, baseUrl, model });
      results.push(result);
      const first = result.attempts[0];
      process.stdout.write(`${first.route_match ? "PASS" : "FAIL"} ${first.total_ms ?? "?"}ms\n`);
    } catch (error) {
      process.stdout.write(`ERROR ${error.message}\n`);
      results.push({
        id: caseDef.id,
        category: caseDef.category,
        expected_route: caseDef.expected_route,
        messages: caseDef.messages,
        attempts: [{ attempt: 1, error: error.message, json_valid: false, route: null, reply: "", route_match: false, reply_check: null }],
      });
    }
  }

  const afterPs = await ollamaPs(baseUrl);
  const residentModels = Array.isArray(afterPs.models) ? afterPs.models : [];
  const resident = residentModels.find((entry) => entry.name === model || entry.model === model) || null;
  const report = {
    schema_version: 1,
    benchmark: fixture.name,
    generated_at: generatedAt,
    git: {
      branch: gitValue(["rev-parse", "--abbrev-ref", "HEAD"]),
      commit: gitValue(["rev-parse", "HEAD"]),
    },
    target: { profile: profileId, base_url: baseUrl, model },
    config: {
      context_size: CONTEXT_SIZE,
      repeats: REPEATS,
      timeout_ms: REQUEST_TIMEOUT_MS,
      temperature: 0.2,
      top_p: 0.9,
      num_predict: 192,
      think: false,
    },
    runtime: { ollama_version: version },
    cold_probe: { unload_succeeded: unloadSucceeded, measurement: coldMeasurement },
    residency: { before: beforePs, after: afterPs, model: resident },
    summary: summarize(results),
    results,
  };

  await fs.mkdir(RESULTS_DIR, { recursive: true });
  const stamp = generatedAt.replace(/[:.]/g, "-");
  const stem = `${safeModelName(model)}__${stamp}`;
  const jsonText = `${JSON.stringify(report, null, 2)}\n`;
  const mdText = markdownReport(report);
  const jsonPath = path.join(RESULTS_DIR, `${stem}.json`);
  const mdPath = path.join(RESULTS_DIR, `${stem}.md`);
  await Promise.all([
    fs.writeFile(jsonPath, jsonText),
    fs.writeFile(mdPath, mdText),
    fs.writeFile(path.join(RESULTS_DIR, "LATEST.json"), jsonText),
    fs.writeFile(path.join(RESULTS_DIR, "LATEST.md"), mdText),
  ]);

  console.log(`\n[xiaomei] report: ${path.relative(SERVER_DIR, mdPath)}`);
  console.log(`[xiaomei] raw:    ${path.relative(SERVER_DIR, jsonPath)}`);
  console.log(`[xiaomei] route accuracy: ${report.summary.route_accuracy_pct}%`);
  console.log(`[xiaomei] median first content: ${report.summary.median_first_content_ms} ms`);
  console.log(`[xiaomei] median total: ${report.summary.median_total_ms} ms`);
}

main().catch((error) => {
  console.error(`[xiaomei] benchmark failed: ${error.stack || error.message}`);
  process.exitCode = 1;
});
