import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";

const HTML_ENTITIES = new Map([
  ["amp", "&"], ["lt", "<"], ["gt", ">"], ["quot", '"'], ["apos", "'"], ["nbsp", " "],
]);

function decodeHtmlEntities(text) {
  return text.replace(/&(#x[0-9a-f]+|#\d+|[a-z]+);/gi, (match, entity) => {
    if (entity[0] === "#") {
      const hex = entity[1]?.toLowerCase() === "x";
      const value = Number.parseInt(entity.slice(hex ? 2 : 1), hex ? 16 : 10);
      return Number.isInteger(value) && value >= 0 && value <= 0x10ffff ? String.fromCodePoint(value) : match;
    }
    return HTML_ENTITIES.get(entity.toLowerCase()) ?? match;
  });
}

/** Convert Telegram HTML into short, natural text without ever speaking markup. */
export function telegramHtmlToSpeech(html, maxChars = 300) {
  let text = String(html ?? "")
    .replace(/<\s*br\s*\/?>/gi, "\n")
    .replace(/<\/(?:blockquote|p|div)>/gi, "\n")
    .replace(/<[^>]*>/g, "");
  text = decodeHtmlEntities(text)
    .replace(/^\s*[a-z][a-z0-9 _-]*:\s*(?:\n|$)/i, "")
    .replace(/\bms\b/gi, "milliseconds")
    .replace(/\bdBm\b/g, "decibels")
    .replace(/\bKB\b/g, "kilobytes")
    .replace(/\bMB\b/g, "megabytes")
    .replace(/[ \t]*\n[ \t]*/g, ". ")
    .replace(/\s+/g, " ")
    .replace(/\.{2,}/g, ".")
    .trim();
  if (text.length <= maxChars) return text;
  const clipped = text.slice(0, maxChars + 1);
  const boundary = clipped.lastIndexOf(" ");
  return `${clipped.slice(0, boundary >= Math.floor(maxChars * 0.7) ? boundary : maxChars).replace(/[.,;:!?\s]+$/, "")}…`;
}

function collectProcessOutput(child, maxBytes, label) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let bytes = 0;
    let stderr = "";
    child.stdout.on("data", (chunk) => {
      bytes += chunk.length;
      if (bytes > maxBytes) { child.kill("SIGKILL"); reject(new Error(`${label} output exceeded limit`)); return; }
      chunks.push(chunk);
    });
    child.stderr.on("data", (chunk) => { stderr = `${stderr}${chunk}`.slice(-2_000); });
    child.once("error", reject);
    child.once("close", (code) => code === 0 ? resolve(Buffer.concat(chunks)) : reject(new Error(`${label} exited ${code}: ${stderr.trim()}`)));
  });
}

/** Replaceable backend boundary. Implementations return raw PCM in the requested format. */
export class EspeakTtsBackend {
  constructor({ voice = "en", rate = 155, maxPcmBytes = 1_920_000 } = {}) {
    this.voice = voice;
    this.rate = rate;
    this.maxPcmBytes = maxPcmBytes;
  }

  async synthesize(text, format) {
    const espeak = spawn("espeak-ng", ["--stdout", "-v", this.voice, "-s", String(this.rate), text], { stdio: ["ignore", "pipe", "pipe"] });
    const ffmpeg = spawn("ffmpeg", ["-hide_banner", "-loglevel", "error", "-i", "pipe:0", "-f", "s16le", "-acodec", "pcm_s16le", "-ac", String(format.channels), "-ar", String(format.sampleRate), "pipe:1"], { stdio: ["pipe", "pipe", "pipe"] });
    espeak.stdout.pipe(ffmpeg.stdin);
    const espeakError = new Promise((_, reject) => {
      let stderr = "";
      espeak.stderr.on("data", (chunk) => { stderr = `${stderr}${chunk}`.slice(-2_000); });
      espeak.once("error", reject);
      espeak.once("close", (code) => { if (code !== 0) reject(new Error(`espeak-ng exited ${code}: ${stderr.trim()}`)); });
    });
    const pcmPromise = collectProcessOutput(ffmpeg, this.maxPcmBytes, "ffmpeg");
    const pcm = await Promise.race([pcmPromise, espeakError]);
    if (pcm.length === 0 || pcm.length % 2 !== 0) throw new Error("TTS returned invalid PCM16 audio");
    return pcm;
  }
}

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

export function createSpeakerRuntime({ logger, backend, enabled, getDevice, sendControl, format = { sampleRate: 16_000, channels: 1, bitsPerSample: 16 }, chunkBytes = 2_048, maxTextChars = 300, resultTimeoutMs = 75_000, maxPendingJobs = 4 }) {
  const jobs = new Map();
  const allJobs = new Set();
  let queue = Promise.resolve();
  let closing = false;

  function settle(job, result) {
    if (job.settled) return;
    job.settled = true;
    clearTimeout(job.timer);
    jobs.delete(job.id);
    allJobs.delete(job);
    result.kind === "complete" ? job.resolve(result) : job.reject(new Error(result.error ?? result.kind));
  }

  async function run(job) {
    if (job.settled || closing) return;
    if (!getDevice()) throw new Error("speaker unavailable");
    const pcm = await backend.synthesize(job.text, format);
    if (!getDevice()) throw new Error("speaker unavailable");
    job.pcm = pcm;
    jobs.set(job.id, job);
    job.timer = setTimeout(() => settle(job, { kind: "timeout", error: "speaker playback timeout" }), resultTimeoutMs);
    job.timer.unref();
    if (!sendControl({ type: "speaker_play", playback_id: job.id, sample_rate: format.sampleRate, channels: format.channels, bits_per_sample: format.bitsPerSample, bytes: pcm.length })) {
      settle(job, { kind: "send_error", error: "speaker control send failed" });
    }
    return job.completion;
  }

  function speak(html, { maxChars = maxTextChars } = {}) {
    if (!enabled || closing) return { kind: "disabled" };
    if (allJobs.size >= maxPendingJobs) return { kind: "busy" };
    const text = telegramHtmlToSpeech(html, maxChars);
    if (!text) return { kind: "empty" };
    if (!getDevice()) return { kind: "offline" };
    const id = randomUUID();
    let resolve;
    let reject;
    const completion = new Promise((res, rej) => { resolve = res; reject = rej; });
    // Callers may intentionally fire-and-forget; keep rejection handled here.
    completion.catch(() => {});
    const job = { id, text, pcm: null, socketClaimed: false, settled: false, timer: null, resolve, reject, completion };
    allJobs.add(job);
    queue = queue.catch(() => {}).then(() => run(job)).catch((error) => {
      settle(job, { kind: "error", error: error?.message ?? "TTS failed" });
      logger.warn({ playback_id: id, error_message: error?.message ?? "unknown" }, "Speaker/TTS job failed");
    });
    return { kind: "queued", playbackId: id, text, completion };
  }

  async function handleConnection(ws, deviceId, playbackId) {
    const job = jobs.get(playbackId);
    if (!job || job.socketClaimed || !job.pcm) { ws.close(4004, "unknown playback"); return; }
    job.socketClaimed = true;
    logger.info({ device_id: deviceId, playback_id: playbackId, bytes: job.pcm.length, format }, "Speaker stream connected");
    try {
      for (let offset = 0; offset < job.pcm.length; offset += chunkBytes) {
        if (ws.readyState !== 1) throw new Error("speaker disconnected");
        const chunk = job.pcm.subarray(offset, Math.min(offset + chunkBytes, job.pcm.length));
        await new Promise((resolve, reject) => ws.send(chunk, { binary: true }, (error) => error ? reject(error) : resolve()));
        // 2,048 mono PCM16 bytes are 64 ms. Gentle pacing bounds receiver memory.
        if (offset + chunk.length < job.pcm.length) await delay(68);
      }
      if (ws.readyState === 1) {
        await new Promise((resolve, reject) => ws.send(JSON.stringify({ type: "speaker_end", playback_id: playbackId, bytes: job.pcm.length }), (error) => error ? reject(error) : resolve()));
        ws.close(1000, "stream complete");
      }
      logger.info({ device_id: deviceId, playback_id: playbackId, bytes: job.pcm.length }, "Speaker PCM stream sent");
    } catch (error) {
      settle(job, { kind: "error", error: error?.message ?? "speaker stream failed" });
      logger.warn({ device_id: deviceId, playback_id: playbackId, error_message: error?.message ?? "unknown" }, "Speaker stream failed");
      if (ws.readyState === 1) ws.close(1011, "speaker stream failed");
    }
  }

  function handleResult(deviceId, message) {
    const job = jobs.get(message.playback_id);
    if (!job) return false;
    const result = message.type === "speaker_complete" ? { kind: "complete", bytes: message.bytes } : { kind: "error", error: message.error ?? "device playback failed" };
    settle(job, result);
    logger[message.type === "speaker_complete" ? "info" : "warn"]({ device_id: deviceId, playback_id: message.playback_id, bytes: message.bytes, error: message.error }, `Speaker playback ${message.type === "speaker_complete" ? "complete" : "failed"}`);
    return true;
  }

  function close() {
    closing = true;
    for (const job of [...allJobs]) settle(job, { kind: "shutdown", error: "server shutting down" });
  }

  return { speak, handleConnection, handleResult, close, format };
}
