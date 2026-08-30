#!/usr/bin/env node
// Host-only Opus packetization benchmark. It never contacts an ESP or server.
import { readFileSync } from "node:fs";
import { createEncoder, Application, Signal } from "libopus-wasm";

const sampleRate = 24_000;
const seconds = 12;
let pcm = process.env.KOKORO_PCM_FILE ? readFileSync(process.env.KOKORO_PCM_FILE) : Buffer.alloc(sampleRate * seconds * 2);
if (pcm.length & 1) throw new Error("PCM fixture must be PCM16");
for (let i = 0; !process.env.KOKORO_PCM_FILE && i < sampleRate * seconds; i += 1) {
  // Deterministic speech-band fixture; packet CPU/size only, not a quality claim.
  const sample = Math.round(9_000 * Math.sin(2 * Math.PI * 173 * i / sampleRate) +
    4_000 * Math.sin(2 * Math.PI * 311 * i / sampleRate));
  pcm.writeInt16LE(sample, i * 2);
}
const median = (values) => [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)];
const cases = [];
for (const frameMs of [20, 40, 60]) for (const bitrate of [20_000, 24_000, 32_000]) {
  const frameSamples = sampleRate * frameMs / 1_000;
  const frameBytes = frameSamples * 2;
  const runs = [];
  for (let run = 0; run < 5; run += 1) {
    const encoder = await createEncoder({ sampleRate, channels: 1, frameSize: frameSamples,
      application: Application.Voip, bitrate, signal: Signal.Voice, vbr: true });
    let payloadBytes = 0;
    const started = performance.now();
    const packetSizes = [];
    for (let offset = 0; offset < pcm.length; offset += frameBytes) {
      const frame = Buffer.alloc(frameBytes); pcm.copy(frame, 0, offset, Math.min(offset + frameBytes, pcm.length));
      const packet = encoder.encode(new Uint8Array(frame.buffer, frame.byteOffset, frameBytes));
      payloadBytes += packet.byteLength; packetSizes.push(packet.byteLength);
    }
    const elapsedMs = performance.now() - started;
    encoder.free();
    runs.push({ elapsedMs, payloadBytes, packetSizes });
  }
  const elapsed = runs.map((run) => run.elapsedMs);
  const payload = runs.map((run) => run.payloadBytes);
  const packets = Math.ceil(pcm.length / frameBytes);
  const sizes = runs[0].packetSizes.sort((a, b) => a - b);
  const p95 = sizes[Math.min(sizes.length - 1, Math.ceil(sizes.length * .95) - 1)];
  cases.push({ frame_ms: frameMs, bitrate, packets_per_second: 1_000 / frameMs,
    packet_count: packets, packetization_delay_ms: frameMs, payload_bytes_median: median(payload),
    packet_bytes_min: sizes[0], packet_bytes_median: median(sizes), packet_bytes_p95: p95, packet_bytes_max: sizes.at(-1),
    wire_bytes_median: median(payload) + packets * 8,
    encode_ms_median: Number(median(elapsed).toFixed(2)), encode_ms_worst: Number(Math.max(...elapsed).toFixed(2)),
    compression_ratio: Number((pcm.length / (median(payload) + packets * 8)).toFixed(2)) });
}
console.log(JSON.stringify({ fixture: process.env.KOKORO_PCM_FILE ? "kokoro_pcm" : "synthetic_tones", fixture_pcm_bytes: pcm.length, seconds: pcm.length / (sampleRate * 2), contended_timing: true, cases }, null, 2));
