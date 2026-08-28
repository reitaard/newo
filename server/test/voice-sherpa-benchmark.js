import { readFile } from "node:fs/promises";
import { performance } from "node:perf_hooks";
import path from "node:path";

import { SherpaAsrBackend } from "../src/voice.js";

const modelDirectory = process.env.VOICE_ASR_MODEL_DIRECTORY
  ?? "models/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17";
const wavFile = process.env.VOICE_ASR_TEST_WAV
  ?? path.join(modelDirectory, "test_wavs/0.wav");

function readPcm16MonoWav(buffer) {
  if (buffer.toString("ascii", 0, 4) !== "RIFF" || buffer.toString("ascii", 8, 12) !== "WAVE") {
    throw new Error("Expected a RIFF/WAVE file");
  }
  let offset = 12;
  let format;
  let pcm;
  while (offset + 8 <= buffer.length) {
    const id = buffer.toString("ascii", offset, offset + 4);
    const size = buffer.readUInt32LE(offset + 4);
    const dataStart = offset + 8;
    if (id === "fmt ") format = { encoding: buffer.readUInt16LE(dataStart), channels: buffer.readUInt16LE(dataStart + 2), sampleRate: buffer.readUInt32LE(dataStart + 4), bitsPerSample: buffer.readUInt16LE(dataStart + 14) };
    if (id === "data") pcm = buffer.subarray(dataStart, dataStart + size);
    offset = dataStart + size + (size % 2);
  }
  if (!format || !pcm || format.encoding !== 1 || format.channels !== 1 || format.sampleRate !== 16_000 || format.bitsPerSample !== 16) {
    throw new Error("Expected mono 16 kHz signed 16-bit PCM WAV");
  }
  return pcm;
}

const memoryBeforeBytes = process.memoryUsage().rss;
const backend = new SherpaAsrBackend({ modelDirectory, numThreads: Number(process.env.VOICE_ASR_THREADS ?? 2) });
const memoryAfterLoadBytes = process.memoryUsage().rss;
const pcm = readPcm16MonoWav(await readFile(wavFile));
const events = [];
const stream = await backend.createStream({
  format: { sampleRate: 16_000, channels: 1, bitsPerSample: 16 },
  onEvent(event) { events.push({ ...event, elapsedMs: performance.now() - startedAt }); },
});
const startedAt = performance.now();
const cpuStart = process.cpuUsage();
for (let offset = 0; offset < pcm.length; offset += 640) await stream.acceptAudio(pcm.subarray(offset, offset + 640));
await stream.stop();
const elapsedMs = performance.now() - startedAt;
const cpu = process.cpuUsage(cpuStart);
const durationSeconds = pcm.length / (16_000 * 2);
const firstPartial = events.find((event) => event.type === "partial");
const final = [...events].reverse().find((event) => event.type === "final");

if (!firstPartial || !final || !final.text.includes("YELLOW LAMPS")) throw new Error(`Streaming ASR regression: ${final?.text ?? "no final transcript"}`);
console.log(JSON.stringify({
  model_directory: modelDirectory,
  wav_file: wavFile,
  audio_duration_s: Number(durationSeconds.toFixed(3)),
  time_to_first_partial_ms: Number(firstPartial.elapsedMs.toFixed(1)),
  elapsed_ms: Number(elapsedMs.toFixed(1)),
  real_time_factor: Number((elapsedMs / 1_000 / durationSeconds).toFixed(3)),
  cpu_ms: Number(((cpu.user + cpu.system) / 1_000).toFixed(1)),
  process_rss_before_model_mib: Number((memoryBeforeBytes / 1024 / 1024).toFixed(1)),
  process_rss_after_model_mib: Number((memoryAfterLoadBytes / 1024 / 1024).toFixed(1)),
  final_transcript: final.text,
}));
