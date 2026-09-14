import { readFileSync } from "node:fs";
import { loadEnvFile } from "node:process";

import { createAssistantRuntime } from "../src/assistant.js";
import { createAssistantProfiles } from "../src/assistant-profiles.js";
import { createAssistantTurnRuntime } from "../src/assistant-turn.js";
import { createAssistantWebTools, readAgentToolsToken } from "../src/assistant-web-tools.js";
import { createStructuredCapabilityRuntime } from "../src/assistant-capabilities.js";
import { createCapabilityRouterClient } from "../src/capability-router.js";
import { PocketTtsBackend } from "../src/pocket-tts.js";

try { loadEnvFile(".env"); } catch (error) { if (error?.code !== "ENOENT") throw error; }

const repetitions = Math.max(1, Math.min(20, Number.parseInt(process.env.WEB_BENCH_REPS || "7", 10)));
const baseUrl = process.env.AGENT_TOOLS_BASE_URL || "http://127.0.0.1:8790";
const token = readAgentToolsToken({ envFile: process.env.AGENT_TOOLS_TOKEN_FILE || "/srv/agent-tools/.env" });
if (!token) throw new Error("AGENT_TOOLS_TOKEN is unavailable; run this benchmark on the VPS with /srv/agent-tools/.env present");

function runtimeOverrides() {
  const path = process.env.RUNTIME_STATE_FILE || "data/runtime-state.json";
  try {
    const state = JSON.parse(readFileSync(path, "utf8"));
    return state?.assistantProfileOverrides && typeof state.assistantProfileOverrides === "object"
      ? state.assistantProfileOverrides : {};
  } catch (error) {
    if (error?.code === "ENOENT") return {};
    throw error;
  }
}

const profiles = createAssistantProfiles({ qwenApiKey: process.env.ASSISTANT_API_KEY || null, overrides: runtimeOverrides() });
const normalTools = createAssistantWebTools({ enabled: true, baseUrl, token,
  timeoutMs: Number.parseInt(process.env.AGENT_TOOLS_TIMEOUT_MS || "12000", 10) });
const timeoutTools = createAssistantWebTools({ enabled: true, baseUrl, token, timeoutMs: 1 });
const pocket = new PocketTtsBackend({ baseUrl: process.env.POCKET_BASE_URL || "http://127.0.0.1:8123",
  voice: process.env.WEB_BENCH_TTS_VOICE || "michael" });
const capabilityRouter = createCapabilityRouterClient({ enabled: process.env.CAPABILITY_ROUTER_ENABLED === "true",
  baseUrl: process.env.CAPABILITY_ROUTER_BASE_URL || "http://127.0.0.1:8791",
  timeoutMs: Number.parseInt(process.env.CAPABILITY_ROUTER_TIMEOUT_MS || "75", 10) });
const structuredCapabilities = createStructuredCapabilityRuntime({
  enabled: process.env.CAPABILITY_FAST_PATH_ENABLED !== "false",
  timeoutMs: Number.parseInt(process.env.CAPABILITY_PROVIDER_TIMEOUT_MS || "3000", 10),
  timeZone: process.env.ASSISTANT_TIME_ZONE || "Asia/Phnom_Penh",
  twelveDataApiKey: process.env.TWELVE_DATA_API_KEY || "",
  apiSportsKey: process.env.API_SPORTS_KEY || "",
});

const cases = [
  { id: "normal_no_tool", text: "What is the capital of France?", tools: normalTools },
  { id: "weather_structured", text: "What is the current weather in Phnom Penh?", tools: normalTools },
  { id: "fx_structured", text: "Convert 100 US dollars to euros using the current reference rate.", tools: normalTools },
  { id: "calculator_local", text: "Calculate (125 plus 75) divided by 4.", tools: normalTools },
  { id: "web_search", text: "Search the web for the latest stable Node.js release and cite the source.", tools: normalTools },
  { id: "web_search_read", text: "Search for the latest Node.js release notes, read the official source, and summarize the main change.", tools: normalTools },
  { id: "fast_web", text: "Search the web for today's Node.js release news.", tools: normalTools },
  { id: "think_web", text: "Current sources conflict about the latest Node.js release. Search and resolve which official source is authoritative.", tools: normalTools },
  { id: "provider_timeout", text: "Search the web for today's Node.js release news.", tools: timeoutTools },
];

const quiet = { info() {}, warn() {} };
const format = { sampleRate: 24_000, channels: 1, bitsPerSample: 16 };
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

class BenchmarkSpeakerRuntime {
  constructor(backend) {
    this.backend = backend;
    this.queue = Promise.resolve();
    this.sequence = 0;
    this.turnStartedAt = performance.now();
    this.events = [];
  }

  beginTurn(at = performance.now()) {
    this.turnStartedAt = at;
    this.events = [];
  }

  snapshot() { return this.events.map((event) => ({ ...event })); }

  #queueSpeech(text, options = {}, segments = null) {
    if (!text) return { kind: "empty" };
    const id = `bench-${++this.sequence}`;
    const metadata = options.metadata ?? {};
    const kind = metadata.progress_feedback ? "progress" : "final";
    let cancelled = false;
    let activeSource = null;
    let resolveCompletion;
    let rejectCompletion;
    const completion = new Promise((resolve, reject) => { resolveCompletion = resolve; rejectCompletion = reject; });
    completion.catch(() => {});

    const run = async () => {
      const speechStartedAt = performance.now();
      this.events.push({ event: "speech_start", kind, at_ms: Math.round(speechStartedAt - this.turnStartedAt),
        progressive: Boolean(segments), text: segments ? "progressive" : text });
      try {
        activeSource = segments
          ? await this.backend.streamSegments(segments, format, { playbackId: id })
          : await this.backend.stream(text, format, { playbackId: id });
        let bytes = 0;
        let firstPcmAt = null;
        for await (const chunk of activeSource.audio) {
          if (cancelled) throw new Error("speaker_cancelled");
          if (!chunk?.length) continue;
          firstPcmAt ??= performance.now();
          if (bytes === 0) this.events.push({ event: "first_pcm", kind,
            at_ms: Math.round(firstPcmAt - this.turnStartedAt) });
          bytes += chunk.length;
        }
        if (cancelled) throw new Error("speaker_cancelled");
        if (!firstPcmAt || bytes === 0) throw new Error("empty_tts_audio");

        // Approximate physical playback completion so progress-feedback blocking
        // is visible even though this benchmark does not connect an ESP speaker.
        const playbackEndAt = firstPcmAt + (bytes / 48);
        const remainingMs = playbackEndAt - performance.now();
        if (remainingMs > 0) await sleep(remainingMs);

        const completedAt = performance.now();
        this.events.push({ event: "speech_complete", kind, at_ms: Math.round(completedAt - this.turnStartedAt),
          pcm_bytes: bytes, estimated_audio_ms: Math.round(bytes / 48) });
        return { kind: "complete", bytes,
          llmStartToFirstAudioMs: metadata.llm_started_at == null ? null : Math.round(firstPcmAt - metadata.llm_started_at) };
      } finally {
        if (cancelled) activeSource?.cancel?.();
      }
    };

    this.queue = this.queue.catch(() => {}).then(run);
    this.queue.then(resolveCompletion, rejectCompletion);
    return { kind: "queued", playbackId: id, text, completion, cancel() {
      cancelled = true;
      activeSource?.cancel?.();
    } };
  }

  speak(text, options = {}) { return this.#queueSpeech(String(text ?? "").trim(), options, null); }
  speakProgressive(segments, options = {}) {
    if (!segments || typeof segments[Symbol.asyncIterator] !== "function") return { kind: "empty" };
    return this.#queueSpeech("progressive", options, segments);
  }
}

function createBenchRuntime(tools) {
  const assistant = createAssistantRuntime({ enabled: true, profiles, preferredProfile: "lfm", webTools: tools,
    capabilityRouter, structuredCapabilities, logger: quiet });
  const speaker = new BenchmarkSpeakerRuntime(pocket);
  const turns = createAssistantTurnRuntime({ assistant, speakerRuntime: speaker,
    isPersistentSpeakerEnabled: () => true, maxReplyChars: 450,
    progressFeedbackEnabled: true, logger: quiet, setAssistantState() {} });
  return { assistant, speaker, turns };
}

const normal = createBenchRuntime(normalTools);
const timeout = createBenchRuntime(timeoutTools);
const rows = [];

function firstEvent(events, event, kind) {
  return events.find((item) => item.event === event && (!kind || item.kind === kind)) ?? null;
}

for (const benchmarkCase of cases) {
  const bench = benchmarkCase.tools === timeoutTools ? timeout : normal;
  for (let repetition = 1; repetition <= repetitions; repetition += 1) {
    const finalAt = performance.now();
    bench.speaker.beginTurn(finalAt);
    const deviceId = `web-bench-${benchmarkCase.id}-${repetition}`;
    const route = bench.assistant.routeRequest({ text: benchmarkCase.text, deviceId });
    const started = bench.turns.handleFinalTranscript({ deviceId, streamId: deviceId, text: benchmarkCase.text, asrFinalMs: 0 });
    const settled = started.kind === "started" ? await started.completion : started;
    const latest = bench.turns.getTelemetry().latest;
    const speakerEvents = bench.speaker.snapshot();
    const firstAnyPcm = firstEvent(speakerEvents, "first_pcm");
    const firstFinalPcm = firstEvent(speakerEvents, "first_pcm", "final");
    const progressComplete = firstEvent(speakerEvents, "speech_complete", "progress");
    const firstFinalStart = firstEvent(speakerEvents, "speech_start", "final");
    const round1 = latest.llmRoundTimings?.[0] ?? null;
    const round2 = latest.llmRoundTimings?.[1] ?? null;
    const localOverhead = latest.agentToolsLatencyMs != null && latest.providerLatencyMs != null
      ? Math.max(0, latest.agentToolsLatencyMs - latest.providerLatencyMs) : null;

    const row = {
      case: benchmarkCase.id, repetition, route: route.route, result: settled?.kind ?? latest.result,
      first_raw_ms: latest.llmFirstRawTokenMs ?? null,
      first_speakable_ms: latest.llmFirstTokenMs ?? null,
      round1_ms: round1?.request_ms ?? null,
      round2_ms: round2?.request_ms ?? null,
      rounds: latest.llmRounds ?? null,
      reasoning_tokens: latest.reasoningTokens ?? null,
      tools: latest.toolSelected ?? [],
      agent_tools_ms: latest.agentToolsLatencyMs ?? null,
      provider_ms: latest.providerLatencyMs ?? null,
      capability: latest.capabilityPrimary ?? null,
      capability_router_ms: latest.capabilityRouterMs ?? null,
      structured_used: latest.structuredCapabilityUsed ?? false,
      structured_tool: latest.structuredTool ?? null,
      structured_provider: latest.structuredProvider ?? null,
      structured_provider_ms: latest.structuredProviderMs ?? null,
      structured_fallback_reason: latest.structuredFallbackReason ?? null,
      local_tool_overhead_ms: localOverhead,
      progress_fired: latest.progressFeedbackFired ?? false,
      progress_cancelled: latest.progressFeedbackCancelled ?? false,
      progress_started_ms: latest.progressFeedbackStartedMs ?? null,
      progress_finished_ms: latest.progressFeedbackFinishedMs ?? null,
      progress_complete_ms: progressComplete?.at_ms ?? null,
      final_tts_start_ms: firstFinalStart?.at_ms ?? null,
      first_any_audio_ms: firstAnyPcm?.at_ms ?? null,
      first_final_audio_ms: firstFinalPcm?.at_ms ?? null,
      llm_ms: latest.llmMs ?? null,
      total_ms: latest.totalMs ?? Math.round(performance.now() - finalAt),
      tool_events: latest.toolEvents ?? [],
      speaker_events: speakerEvents,
    };
    rows.push(row);
    console.log(JSON.stringify(row));
  }
}

function percentile(values, p) {
  const sorted = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!sorted.length) return null;
  return sorted[Math.max(0, Math.ceil((p / 100) * sorted.length) - 1)];
}

function stats(values) { return { p50: percentile(values, 50), p90: percentile(values, 90) }; }
const metrics = [
  ["first_raw_ms", "first raw token"],
  ["first_speakable_ms", "first speakable token"],
  ["round1_ms", "LFM round 1"],
  ["agent_tools_ms", "agent-tools total"],
  ["provider_ms", "provider"],
  ["capability_router_ms", "capability router"],
  ["structured_provider_ms", "structured provider"],
  ["local_tool_overhead_ms", "agent-tools/local overhead"],
  ["round2_ms", "LFM round 2"],
  ["first_final_audio_ms", "first final PCM"],
  ["total_ms", "turn complete"],
];

console.log("\n=== WEB BENCHMARK SUMMARY (ms) ===");
for (const benchmarkCase of cases) {
  const samples = rows.filter((row) => row.case === benchmarkCase.id);
  console.log(`\n${benchmarkCase.id}  n=${samples.length}`);
  for (const [key, label] of metrics) {
    const { p50, p90 } = stats(samples.map((row) => row[key]));
    if (p50 == null) continue;
    console.log(`${label.padEnd(28)} p50=${String(p50).padStart(5)}  p90=${String(p90).padStart(5)}`);
  }
  const fired = samples.filter((row) => row.progress_fired).length;
  const cancelled = samples.filter((row) => row.progress_cancelled).length;
  console.log(`progress feedback            fired=${fired}/${samples.length}  cancelled=${cancelled}/${samples.length}`);
}

console.log("\nNotes:");
console.log("- first_final_audio_ms is server-side Pocket PCM readiness, not measured ESP acoustic onset.");
console.log("- benchmark speaker simulates real-time playback duration so progress-speech blocking is visible.");
console.log("- agent-tools/local overhead is agent-tools elapsed minus provider-reported elapsed when both exist.");

normal.turns.close();
timeout.turns.close();
