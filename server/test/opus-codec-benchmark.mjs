#!/usr/bin/env node
// Host-only Opus packetization benchmark. It never contacts an ESP or server.
import { createEncoder, Application, Signal } from "libopus-wasm";

const sampleRate = 24_000;
const seconds = 12;
const pcm = Buffer.alloc(sampleRate * seconds * 2);
for (let i = 0; i < sampleRate * seconds; i += 1) {
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
    for (let offset = 0; offset < pcm.length; offset += frameBytes) {
      const packet = encoder.encode(new Uint8Array(pcm.buffer, pcm.byteOffset + offset, frameBytes));
      payloadBytes += packet.byteLength;
    }
    const elapsedMs = performance.now() - started;
    encoder.free();
    runs.push({ elapsedMs, payloadBytes });
  }
  const elapsed = runs.map((run) => run.elapsedMs);
  const payload = runs.map((run) => run.payloadBytes);
  const packets = pcm.length / frameBytes;
  cases.push({ frame_ms: frameMs, bitrate, packets_per_second: 1_000 / frameMs,
    packets_per_10s: packets, payload_bytes_median: median(payload),
    wire_bytes_median: median(payload) + packets * 8,
    encode_ms_median: Number(median(elapsed).toFixed(2)), encode_ms_worst: Number(Math.max(...elapsed).toFixed(2)),
    compression_ratio: Number((pcm.length / (median(payload) + packets * 8)).toFixed(2)) });
}
console.log(JSON.stringify({ fixture_pcm_bytes: pcm.length, seconds, cases }, null, 2));
