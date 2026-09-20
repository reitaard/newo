import { runBenchmark } from "./memory-bench-core.mjs";

const minimal = await runBenchmark("minimal");
const voicemem = await runBenchmark("voicemem");

console.log("\n=== MEMORY EXTRACTION COMPARISON ===");
for (const mode of new Set([...minimal.runs.map(x => x.mode), ...voicemem.runs.map(x => x.mode)])) {
  for (const report of [minimal, voicemem]) {
    const run = report.runs.find(x => x.mode === mode);
    if (!run) continue;
    const s = run.summary;
    console.log(`${report.style.padEnd(9)} ${mode.padEnd(5)} F1=${(s.fact_f1 * 100).toFixed(1)}% false=${s.false_memories} JSON=${(s.json_valid_rate * 100).toFixed(1)}% p50=${s.total_latency_ms.p50}ms p95=${s.total_latency_ms.p95}ms`);
  }
}
