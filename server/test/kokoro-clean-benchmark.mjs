#!/usr/bin/env node
// Run after closing Recode: KOKORO_URL=http://127.0.0.1:8010 node test/kokoro-clean-benchmark.mjs
import os from "node:os";
import process from "node:process";
import { performance } from "node:perf_hooks";
import { splitRealtimeText, conditionPcm16 } from "../src/tts.js";

const url = process.env.KOKORO_URL ?? "http://127.0.0.1:8010";
const runs = Number(process.env.RUNS ?? 5);
const texts = {
  short: "Certainly, I can help with that right now.",
  medium: "Hello, I am Newo, and I can explain the current speaker status clearly before we make any changes.",
  long: "The speaker is connected and ready. I will keep the audio stream bounded, preserve exact completion accounting, and report any error instead of guessing about playback progress.",
  comma_heavy: "First, check the connection, then verify the buffer, next inspect the decoder, and finally confirm that completion is exact.",
  punctuation_free: "please describe the current speaker connection buffer decoder completion state and explain what should happen next without adding unnecessary detail",
  assistant: "Hello, I'm Newo. I can help you with that. The first step is to verify the speaker connection and then continue safely.",
};
const percentile = (values, p) => { const sorted = [...values].sort((a, b) => a - b); return sorted[Math.min(sorted.length - 1, Math.ceil(sorted.length * p) - 1)]; };
const summary = (values) => ({ median: Number(percentile(values, .5).toFixed(1)), p90: Number(percentile(values, .9).toFixed(1)), worst: Number(Math.max(...values).toFixed(1)) });
const contended = process.argv.includes("--contended") || os.loadavg()[0] > os.cpus().length * .75 || process.env.RECODE_SESSION != null;
console.log(JSON.stringify({ benchmark: "kokoro-clean-segmentation", warning: contended ? "CONTENDED BENCHMARK — RESULTS NOT AUTHORITATIVE" : null, timestamp: new Date().toISOString(), cpu_count: os.cpus().length, loadavg: os.loadavg(), free_ram: os.freemem(), url, runs }, null, 2));
for (const target of [20, 28, 36, 44]) for (const [name, text] of Object.entries(texts)) {
  const segments = splitRealtimeText(text, { firstSegmentTargetChars: target }); const samples = [];
  for (let run = 0; run < runs; run += 1) {
    const started = performance.now(); let firstRaw = null, firstConditioned = null, pcmBytes = 0; const segmentEvents = [];
    for (const segment of segments) {
      const segmentStarted = performance.now();
      const response = await fetch(`${url}/v1/audio/realtime`, { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ model: "kokoro", input: segment, voice: "am_michael", response_format: "pcm", speed: 1 }) });
      if (!response.ok || !response.body) throw new Error(`Kokoro HTTP ${response.status}`);
      const chunks = []; for await (const chunk of response.body) { if (firstRaw === null) firstRaw = performance.now(); chunks.push(Buffer.from(chunk)); }
      const conditioned = await conditionPcm16(Buffer.concat(chunks), { sampleRate: 24000, channels: 1, bitsPerSample: 16 });
      if (firstConditioned === null) firstConditioned = performance.now(); pcmBytes += conditioned.length;
      segmentEvents.push({ start_ms: segmentStarted - started, first_pcm_ms: firstRaw - started, complete_ms: performance.now() - started });
    }
    samples.push({ raw: firstRaw - started, conditioned: firstConditioned - started, total: performance.now() - started, audio_ms: pcmBytes / 48, rtf: (performance.now() - started) / (pcmBytes / 48), gaps: segmentEvents.slice(1).map((event, i) => event.start_ms - segmentEvents[i].complete_ms) });
  }
  console.log(JSON.stringify({ target, case: name, first_segment: segments[0], first_segment_chars: segments[0].length, segment_count: segments.length,
    first_raw_ms: summary(samples.map((x) => x.raw)), first_conditioned_ms: summary(samples.map((x) => x.conditioned)), total_generation_ms: summary(samples.map((x) => x.total)), audio_ms: summary(samples.map((x) => x.audio_ms)), rtf: summary(samples.map((x) => x.rtf)), inter_segment_gap_ms: summary(samples.flatMap((x) => x.gaps.length ? x.gaps : [0])) }));
}
