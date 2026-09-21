import { promises as fs } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SERVER_DIR = path.resolve(__dirname, "..");
const OUT_DIR = path.join(SERVER_DIR, "benchmarks", "xiaomei", "results");
const BASE_URL = (process.env.XIAOMEI_AUDIT_BASE_URL || "http://100.110.136.15:11435").replace(/\/api\/chat\/?$/, "").replace(/\/$/, "");
const MODEL_LIST = (process.env.XIAOMEI_AUDIT_MODELS || [
  "newo-qwen35-2b:latest",
  "newo-qwen38-2b:latest",
  "newo-minicpm5:latest",
  "newo-gemma-e2b:latest",
  "newo-spark17:latest",
  "newo-spark-x2.5-4b:archive",
].join(","))
  .split(",")
  .map((value) => value.trim())
  .filter(Boolean);

const SYSTEM_PROMPT = [
  "You are a Xiaomei benchmark probe.",
  "Return exactly one JSON object and nothing else:",
  '{"route":"chat","reply":"OK"}',
].join(" ");

const MODES = [
  { id: "native", think: undefined },
  { id: "off", think: false },
  { id: "on", think: true },
];

function round(value, digits = 1) {
  return Number.isFinite(value) ? Number(value.toFixed(digits)) : null;
}

async function fetchJson(url, init = {}, timeoutMs = 90000) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, { ...init, signal: controller.signal });
    const text = await response.text();
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}: ${text.slice(0, 1000)}`);
    return text ? JSON.parse(text) : {};
  } finally {
    clearTimeout(timer);
  }
}

async function showModel(model) {
  return fetchJson(`${BASE_URL}/api/show`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ model, verbose: true }),
  });
}

async function unload(model) {
  try {
    await fetchJson(`${BASE_URL}/api/generate`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ model, prompt: "", stream: false, keep_alive: 0 }),
    }, 30000);
  } catch {}
}

function stripThinkBlocks(text) {
  return String(text || "")
    .replace(/<think>[\s\S]*?<\/think>/gi, "")
    .replace(/<\|channel\>thought[\s\S]*?<channel\|>/gi, "")
    .trim();
}

function parseJson(text) {
  const cleaned = stripThinkBlocks(text).replace(/^```(?:json)?\s*/i, "").replace(/\s*```$/i, "").trim();
  const candidates = [cleaned];
  const first = cleaned.indexOf("{");
  const last = cleaned.lastIndexOf("}");
  if (first >= 0 && last > first) candidates.push(cleaned.slice(first, last + 1));
  for (const candidate of candidates) {
    try {
      const value = JSON.parse(candidate);
      return { valid: true, value };
    } catch {}
  }
  return { valid: false, value: null };
}

async function probe(model, mode) {
  await unload(model);
  const body = {
    model,
    messages: [
      { role: "system", content: SYSTEM_PROMPT },
      { role: "user", content: "Return the required JSON now." },
    ],
    stream: false,
    keep_alive: -1,
    options: {
      num_ctx: 4096,
      temperature: 1.0,
      top_p: 0.95,
      top_k: 20,
      num_predict: 2048,
    },
  };
  if (mode.think !== undefined) body.think = mode.think;

  const started = performance.now();
  try {
    const response = await fetchJson(`${BASE_URL}/api/chat`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    });
    const totalMs = performance.now() - started;
    const content = String(response.message?.content || "");
    const thinking = String(response.message?.thinking || "");
    const parsed = parseJson(content);
    const evalSec = Number(response.eval_duration || 0) / 1e9;
    return {
      mode: mode.id,
      requested_think: mode.think ?? "omitted",
      ok: true,
      total_ms: round(totalMs),
      load_ms: round(Number(response.load_duration || 0) / 1e6),
      prompt_eval_count: response.prompt_eval_count ?? null,
      eval_count: response.eval_count ?? null,
      decode_tps: evalSec > 0 ? round((response.eval_count || 0) / evalSec) : null,
      done_reason: response.done_reason ?? null,
      thinking_chars: thinking.length,
      content_chars: content.length,
      content_has_think_tag: /<think>|<\|channel\>thought/i.test(content),
      json_valid_after_think_strip: parsed.valid,
      parsed_route: parsed.value?.route ?? null,
      raw_thinking: thinking,
      raw_content: content,
    };
  } catch (error) {
    return {
      mode: mode.id,
      requested_think: mode.think ?? "omitted",
      ok: false,
      error: error.message,
    };
  }
}

function summarizeShow(show) {
  const template = String(show.template || "");
  const params = String(show.parameters || "");
  const details = show.details || {};
  return {
    family: details.family || details.families?.[0] || null,
    parameter_size: details.parameter_size || null,
    quantization_level: details.quantization_level || null,
    format: details.format || null,
    parameters: params,
    system: show.system || "",
    template: template,
    template_has_think: /think/i.test(template),
    template_has_enable_thinking: /enable_thinking/i.test(template),
  };
}

function markdown(report) {
  const lines = [];
  lines.push("# Xiaomei Model Configuration Audit", "");
  lines.push(`- Generated: ${report.generated_at}`);
  lines.push(`- Endpoint: \`${report.endpoint}\``);
  lines.push("- Probe context: 4096");
  lines.push("- Probe output budget: 2048 tokens");
  lines.push("- Probe sampling: temperature 1.0 / top_p 0.95 / top_k 20", "");
  lines.push("## Mode behavior", "");
  lines.push("| Model | Family | Quant | Mode | Think request | Thinking chars | Content <think> | JSON after strip | Total ms | Decode t/s | Done |");
  lines.push("| --- | --- | --- | --- | --- | ---: | :---: | :---: | ---: | ---: | --- |");
  for (const model of report.models) {
    for (const probe of model.probes) {
      lines.push(`| ${model.model} | ${model.show.family || "?"} | ${model.show.quantization_level || "?"} | ${probe.mode} | ${String(probe.requested_think)} | ${probe.thinking_chars ?? "n/a"} | ${probe.content_has_think_tag ? "yes" : "no"} | ${probe.json_valid_after_think_strip ? "yes" : "no"} | ${probe.total_ms ?? "n/a"} | ${probe.decode_tps ?? "n/a"} | ${probe.done_reason || probe.error || "n/a"} |`);
    }
  }
  lines.push("", "## Ollama model configuration", "");
  for (const model of report.models) {
    lines.push(`### ${model.model}`, "");
    lines.push(`- family: ${model.show.family || "unknown"}`);
    lines.push(`- parameter size: ${model.show.parameter_size || "unknown"}`);
    lines.push(`- quantization: ${model.show.quantization_level || "unknown"}`);
    lines.push(`- template contains think controls: ${model.show.template_has_think}`);
    lines.push(`- template contains enable_thinking: ${model.show.template_has_enable_thinking}`);
    lines.push("- parameters:");
    lines.push("```text");
    lines.push(model.show.parameters || "(none reported)");
    lines.push("```");
  }
  lines.push("", "## Interpretation", "");
  lines.push("Do not compare controller quality until each candidate has a validated native FAST/no-think path or is explicitly classified as reasoning-only. A model that ignores `think:false`, emits reasoning inside content, or truncates before its final JSON must not be scored as a normal FAST failure.");
  return `${lines.join("\n")}\n`;
}

async function main() {
  await fs.mkdir(OUT_DIR, { recursive: true });
  const report = {
    schema_version: 1,
    generated_at: new Date().toISOString(),
    endpoint: BASE_URL,
    models: [],
  };

  for (const model of MODEL_LIST) {
    process.stdout.write(`[xiaomei-audit] ${model} / show ... `);
    let show;
    try {
      show = summarizeShow(await showModel(model));
      process.stdout.write("OK\n");
    } catch (error) {
      process.stdout.write(`ERROR ${error.message}\n`);
      report.models.push({ model, show_error: error.message, show: {}, probes: [] });
      continue;
    }

    const probes = [];
    for (const mode of MODES) {
      process.stdout.write(`[xiaomei-audit] ${model} / ${mode.id} ... `);
      const result = await probe(model, mode);
      probes.push(result);
      if (result.ok) {
        process.stdout.write(`JSON=${result.json_valid_after_think_strip ? "yes" : "no"} think=${result.thinking_chars} contentThink=${result.content_has_think_tag ? "yes" : "no"} ${result.total_ms}ms\n`);
      } else {
        process.stdout.write(`ERROR ${result.error}\n`);
      }
    }
    report.models.push({ model, show, probes });
  }

  const jsonText = `${JSON.stringify(report, null, 2)}\n`;
  const mdText = markdown(report);
  await Promise.all([
    fs.writeFile(path.join(OUT_DIR, "MODEL-CONFIG-AUDIT-LATEST.json"), jsonText),
    fs.writeFile(path.join(OUT_DIR, "MODEL-CONFIG-AUDIT-LATEST.md"), mdText),
  ]);
  console.log(`\n[xiaomei-audit] report: benchmarks/xiaomei/results/MODEL-CONFIG-AUDIT-LATEST.md`);
  console.log(`[xiaomei-audit] raw:    benchmarks/xiaomei/results/MODEL-CONFIG-AUDIT-LATEST.json`);
}

main().catch((error) => {
  console.error(`[xiaomei-audit] failed: ${error.stack || error.message}`);
  process.exitCode = 1;
});
