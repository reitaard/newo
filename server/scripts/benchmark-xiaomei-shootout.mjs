import { execFileSync, spawnSync } from "node:child_process";
import { promises as fs } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SERVER_DIR = path.resolve(__dirname, "..");
const CONFIG_PATH = path.join(SERVER_DIR, "benchmarks", "xiaomei", "candidates.json");
const RESULTS_DIR = path.join(SERVER_DIR, "benchmarks", "xiaomei", "results");
const SINGLE_SCRIPT = path.join(SERVER_DIR, "scripts", "benchmark-xiaomei-controller.mjs");

function gitValue(args) {
  try {
    return execFileSync("git", args, { cwd: path.resolve(SERVER_DIR, ".."), encoding: "utf8" }).trim();
  } catch {
    return null;
  }
}

function baseUrlFromEndpoint(value) {
  return String(value || "").trim().replace(/\/api\/chat\/?$/i, "").replace(/\/$/, "");
}

async function readConfig() {
  const config = JSON.parse(await fs.readFile(CONFIG_PATH, "utf8"));
  if (!Array.isArray(config.candidates) || config.candidates.length === 0) throw new Error("Xiaomei candidates.json contains no candidates");
  if (!config.endpoint) throw new Error("Xiaomei candidates.json is missing endpoint");
  for (const candidate of config.candidates) {
    if (!candidate.label || !candidate.model) throw new Error("Every Xiaomei candidate requires label and model");
  }
  return config;
}

async function unload(baseUrl, model) {
  try {
    const response = await fetch(`${baseUrl}/api/generate`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ model, prompt: "", stream: false, keep_alive: 0 }),
    });
    return response.ok;
  } catch {
    return false;
  }
}

function markdownReport(report) {
  const lines = [];
  lines.push("# Xiaomei Controller Shootout");
  lines.push("");
  lines.push(`- Generated: ${report.generated_at}`);
  lines.push(`- Branch: ${report.git.branch || "unknown"}`);
  lines.push(`- Commit: ${report.git.commit || "unknown"}`);
  lines.push(`- Endpoint: \`${report.endpoint}\``);
  lines.push(`- Context: ${report.context_size}`);
  lines.push("");
  lines.push("## Comparison");
  lines.push("");
  lines.push("| Candidate | Model | Route | JSON | Reply | First ms | Total ms | Prefill t/s | Decode t/s | VRAM | Status |");
  lines.push("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |");
  for (const entry of report.candidates) {
    if (entry.status !== "ok") {
      lines.push(`| ${entry.label} | \`${entry.model}\` | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | ERROR |`);
      continue;
    }
    const summary = entry.report.summary;
    const vram = entry.report.residency?.model?.size_vram ?? "n/a";
    lines.push(`| ${entry.label} | \`${entry.model}\` | ${summary.route_accuracy_pct ?? "n/a"}% | ${summary.json_valid_pct ?? "n/a"}% | ${summary.reply_check_pct ?? "n/a"}% | ${summary.median_first_content_ms ?? "n/a"} | ${summary.median_total_ms ?? "n/a"} | ${summary.median_prompt_eval_tps ?? "n/a"} | ${summary.median_decode_tps ?? "n/a"} | ${vram} | OK |`);
  }
  lines.push("");
  lines.push("## Review rule");
  lines.push("");
  lines.push("Do not select a winner from speed alone. Review each candidate's timestamped JSON for Chinese quality, code switching, teaching quality, interpreter/meta-command judgment, and voice-response brevity before deciding.");
  lines.push("");
  lines.push("## Individual reports");
  lines.push("");
  for (const entry of report.candidates) {
    if (entry.status === "ok") lines.push(`- ${entry.label}: ${entry.report_file}`);
    else lines.push(`- ${entry.label}: ERROR — ${entry.error}`);
  }
  lines.push("");
  return `${lines.join("\n")}\n`;
}

async function main() {
  const config = await readConfig();
  const baseUrl = baseUrlFromEndpoint(config.endpoint);
  if (!baseUrl) throw new Error("Could not derive Ollama base URL from candidates.json endpoint");

  await fs.mkdir(RESULTS_DIR, { recursive: true });
  const combined = {
    schema_version: 1,
    benchmark: "xiaomei-controller-shootout",
    generated_at: new Date().toISOString(),
    git: {
      branch: gitValue(["rev-parse", "--abbrev-ref", "HEAD"]),
      commit: gitValue(["rev-parse", "HEAD"]),
    },
    endpoint: config.endpoint,
    base_url: baseUrl,
    context_size: 4096,
    candidates: [],
  };

  for (const candidate of config.candidates) {
    console.log(`\n[xiaomei-shootout] === ${candidate.label} / ${candidate.model} ===`);
    const child = spawnSync(process.execPath, [SINGLE_SCRIPT], {
      cwd: SERVER_DIR,
      stdio: "inherit",
      env: {
        ...process.env,
        XIAOMEI_BENCH_BASE_URL: baseUrl,
        XIAOMEI_BENCH_MODEL: candidate.model,
      },
    });

    if (child.status !== 0) {
      combined.candidates.push({
        label: candidate.label,
        model: candidate.model,
        status: "error",
        error: child.error?.message || `benchmark exited with status ${child.status}`,
      });
      await unload(baseUrl, candidate.model);
      continue;
    }

    const latestPath = path.join(RESULTS_DIR, "LATEST.json");
    const report = JSON.parse(await fs.readFile(latestPath, "utf8"));
    if (report.target?.model !== candidate.model) throw new Error(`LATEST.json model mismatch after ${candidate.label}`);
    report.target.label = candidate.label;
    const sourceStamp = String(report.generated_at).replace(/[:.]/g, "-");
    const reportFile = `${String(candidate.model).replace(/[^a-zA-Z0-9._-]+/g, "_").slice(0, 120)}__${sourceStamp}.md`;
    combined.candidates.push({ label: candidate.label, model: candidate.model, status: "ok", report_file: reportFile, report });
    await unload(baseUrl, candidate.model);
  }

  const stamp = combined.generated_at.replace(/[:.]/g, "-");
  const jsonText = `${JSON.stringify(combined, null, 2)}\n`;
  const mdText = markdownReport(combined);
  await Promise.all([
    fs.writeFile(path.join(RESULTS_DIR, `SHOOTOUT__${stamp}.json`), jsonText),
    fs.writeFile(path.join(RESULTS_DIR, `SHOOTOUT__${stamp}.md`), mdText),
    fs.writeFile(path.join(RESULTS_DIR, "SHOOTOUT-LATEST.json"), jsonText),
    fs.writeFile(path.join(RESULTS_DIR, "SHOOTOUT-LATEST.md"), mdText),
  ]);

  console.log("\n[xiaomei-shootout] combined report: benchmarks/xiaomei/results/SHOOTOUT-LATEST.md");
  if (combined.candidates.some((entry) => entry.status !== "ok")) process.exitCode = 1;
}

main().catch((error) => {
  console.error(`[xiaomei-shootout] failed: ${error.stack || error.message}`);
  process.exitCode = 1;
});
