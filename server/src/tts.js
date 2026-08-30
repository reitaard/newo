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

// One-pass, low-latency speech conditioning: remove inaudible small-speaker bass
// and catch peaks without the alimiter auto-gain raising the noise floor.
export const SPEAKER_AUDIO_FILTER = "highpass=f=110,alimiter=limit=0.92:attack=5:release=50:level=false:latency=true";
export const SPEAKER_INITIAL_LEAD_BYTES = 8_192;

/** Replaceable backend boundary. Implementations return raw PCM in the requested format. */
export class EspeakTtsBackend {
  constructor({ voice = "en", rate = 155, maxPcmBytes = 1_920_000 } = {}) {
    this.voice = voice;
    this.rate = rate;
    this.maxPcmBytes = maxPcmBytes;
  }

  async synthesize(text, format) {
    const espeak = spawn("espeak-ng", ["--stdout", "-v", this.voice, "-s", String(this.rate), text], { stdio: ["ignore", "pipe", "pipe"] });
    const ffmpeg = spawn("ffmpeg", ["-hide_banner", "-loglevel", "error", "-i", "pipe:0", "-af", SPEAKER_AUDIO_FILTER, "-f", "s16le", "-acodec", "pcm_s16le", "-ac", String(format.channels), "-ar", String(format.sampleRate), "pipe:1"], { stdio: ["pipe", "pipe", "pipe"] });
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

export function speakerChunkDueMs(bytesSent, leadBytes, bytesPerSecond) {
  if (!Number.isFinite(bytesPerSecond) || bytesPerSecond <= 0) throw new Error("invalid PCM byte rate");
  return Math.max(0, (bytesSent - leadBytes) * 1_000 / bytesPerSecond);
}

export function createSpeakerRuntime({ logger, backend, enabled, getDevice, sendControl, format = { sampleRate: 16_000, channels: 1, bitsPerSample: 16 }, chunkBytes = 1_024, maxTextChars = 300, connectionTimeoutMs = 9_000, resultTimeoutMs = 75_000, maxPendingJobs = 4 }) {
  const jobs = new Map();
  const allJobs = new Set();
  let queue = Promise.resolve();
  let closing = false;

  function settle(job, result) {
    if (job.settled) return;
    job.settled = true;
    clearTimeout(job.connectionTimer);
    clearTimeout(job.resultTimer);
    jobs.delete(job.id);
    allJobs.delete(job);
    result.kind === "complete" ? job.resolve(result) : job.reject(new Error(result.error ?? result.kind));
  }

  function controlMessage(job) {
    return { type: "speaker_play", playback_id: job.id, sample_rate: format.sampleRate, channels: format.channels, bits_per_sample: format.bitsPerSample, bytes: job.pcm.length };
  }

  async function issueControl(job, device, reason) {
    const sent = await sendControl(controlMessage(job), device);
    if (sent) {
      job.controlDevice = device;
      logger.info({ playback_id: job.id, bytes: job.pcm.length, reason }, "Speaker play control sent");
    } else {
      logger.warn({ playback_id: job.id, bytes: job.pcm.length, reason }, "Speaker play control send failed");
    }
    return sent;
  }

  async function run(job) {
    if (job.settled || closing) return;
    if (!getDevice()) throw new Error("speaker unavailable");
    logger.info({ playback_id: job.id, text_chars: job.text.length }, "Speaker TTS synthesis started");
    const pcm = await backend.synthesize(job.text, format);
    logger.info({ playback_id: job.id, pcm_bytes: pcm.length, format }, "Speaker TTS synthesis completed");
    const device = getDevice();
    if (!device) throw new Error("speaker unavailable");
    job.pcm = pcm;
    jobs.set(job.id, job);
    job.connectionTimer = setTimeout(() => {
      settle(job, { kind: "connection_timeout", error: "speaker connection timeout" });
      logger.warn({ playback_id: job.id, pcm_bytes: pcm.length, timeout_ms: connectionTimeoutMs }, "Speaker stream did not connect");
    }, connectionTimeoutMs);
    job.connectionTimer.unref();
    // A control send can race a /device reconnect. Keep the short connection
    // timer alive so handleDeviceConnected can retry on the replacement socket.
    await issueControl(job, device, "initial");
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
    const job = { id, text, pcm: null, socketClaimed: false, settled: false, connectionTimer: null, resultTimer: null, controlDevice: null, controlSending: false, resolve, reject, completion };
    allJobs.add(job);
    logger.info({ playback_id: id, text_chars: text.length }, "Speaker job queued");
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
    clearTimeout(job.connectionTimer);
    job.resultTimer = setTimeout(() => settle(job, { kind: "timeout", error: "speaker playback timeout" }), resultTimeoutMs);
    job.resultTimer.unref();
    const bytesPerSecond = format.sampleRate * format.channels * (format.bitsPerSample / 8);
    const leadBytes = Math.min(SPEAKER_INITIAL_LEAD_BYTES, job.pcm.length);
    logger.info({ device_id: deviceId, playback_id: playbackId, pcm_bytes: job.pcm.length, format }, "Speaker stream connected");
    logger.info({ device_id: deviceId, playback_id: playbackId, pcm_bytes: job.pcm.length, chunk_bytes: chunkBytes, lead_bytes: leadBytes, bytes_per_second: bytesPerSecond }, "Speaker PCM stream started");
    const streamStartedAt = performance.now();
    try {
      for (let offset = 0; offset < job.pcm.length; offset += chunkBytes) {
        if (ws.readyState !== 1) throw new Error("speaker disconnected");
        const chunk = job.pcm.subarray(offset, Math.min(offset + chunkBytes, job.pcm.length));
        const dueAt = streamStartedAt + speakerChunkDueMs(offset + chunk.length, leadBytes, bytesPerSecond);
        const waitMs = dueAt - performance.now();
        if (waitMs > 0) await delay(waitMs);
        await new Promise((resolve, reject) => ws.send(chunk, { binary: true }, (error) => error ? reject(error) : resolve()));
      }
      if (ws.readyState === 1) {
        await new Promise((resolve, reject) => ws.send(JSON.stringify({ type: "speaker_end", playback_id: playbackId, bytes: job.pcm.length }), (error) => error ? reject(error) : resolve()));
        ws.close(1000, "stream complete");
      }
      logger.info({ device_id: deviceId, playback_id: playbackId, bytes: job.pcm.length, stream_ms: Math.round(performance.now() - streamStartedAt) }, "Speaker PCM stream sent");
    } catch (error) {
      settle(job, { kind: "error", error: error?.message ?? "speaker stream failed" });
      logger.warn({ device_id: deviceId, playback_id: playbackId, error_message: error?.message ?? "unknown" }, "Speaker stream failed");
      if (ws.readyState === 1) ws.close(1011, "speaker stream failed");
    }
  }

  async function handleDeviceConnected(deviceId, device) {
    for (const job of jobs.values()) {
      if (job.settled || job.socketClaimed || job.controlSending || job.controlDevice === device) continue;
      job.controlSending = true;
      try {
        if (!await issueControl(job, device, "device_reconnected")) {
          logger.warn({ device_id: deviceId, playback_id: job.id }, "Speaker control retry failed");
        }
      } catch (error) {
        logger.warn({ device_id: deviceId, playback_id: job.id, error_message: error?.message ?? "unknown" }, "Speaker control retry failed");
      } finally {
        job.controlSending = false;
      }
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

  return { speak, handleConnection, handleDeviceConnected, handleResult, close, format };
}
