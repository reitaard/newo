import { runBenchmark } from "./memory-bench-core.mjs";

const minimal = await runBenchmark("minimal");
const voicemem = await runBenchmark("voicemem");

console.log("\n=== MEMORY EXTRACTION COMPARISON ===");
for (const mode of new Set([...minimal.runs.map(x => x.mode), ...voicemem.runs.map(x => x.mode)])) {
  for (const report of [minimal, voicemem]) {
    const run = report.runs.find(x => x.mode === mode);
    if (!run) continue;
    const s = run.summary;
    if (!s.valid_cases) {
      console.log(`${report.style.padEnd(9)} ${mode.padEnd(5)} INVALID valid=0/${s.cases} request_errors=${s.request_errors} parse_errors=${s.parse_errors} first_error=${s.first_error ?? "unknown"}`);
      continue;
    }
    const f1 = s.fact_f1 == null ? "n/a" : `${(s.fact_f1 * 100).toFixed(1)}%`;
    const falseMemories = s.false_memories == null ? "n/a" : s.false_memories;
    console.log(`${report.style.padEnd(9)} ${mode.padEnd(5)} F1=${f1} false=${falseMemories} JSON=${(s.json_valid_rate * 100).toFixed(1)}% valid=${s.valid_cases}/${s.cases} reqErr=${s.request_errors} parseErr=${s.parse_errors} p50=${s.total_latency_ms.p50}ms p95=${s.total_latency_ms.p95}ms`);
  }
}
