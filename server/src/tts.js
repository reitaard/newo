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

export const SPEAKER_GAIN_DB = 6;
export const SPEAKER_LIMITER = 0.95;
export const SPEAKER_LIMITER_ATTACK_MS = 5;
export const SPEAKER_INITIAL_LEAD_BYTES = 8_192;
export const SPEAKER_TARGET_OUTSTANDING_BYTES = 8_192;
export const SPEAKER_MAX_OUTSTANDING_BYTES = 12_288;
export const SPEAKER_FLOW_TIMEOUT_MS = 3_000;

export function speakerAudioFilter(gainDb = SPEAKER_GAIN_DB, limiter = SPEAKER_LIMITER) {
  if (!Number.isFinite(gainDb) || gainDb < -12 || gainDb > 18) throw new Error("invalid speaker gain");
  if (!Number.isFinite(limiter) || limiter <= 0 || limiter > 1) throw new Error("invalid speaker limiter");
  return `highpass=f=110,volume=${gainDb}dB:precision=float,alimiter=limit=${limiter}:attack=${SPEAKER_LIMITER_ATTACK_MS}:release=50:level=false:latency=true`;
}

export const SPEAKER_AUDIO_FILTER = speakerAudioFilter();

export function analyzePcm16(pcm, limiter = SPEAKER_LIMITER) {
  if (!Buffer.isBuffer(pcm) || pcm.length === 0 || (pcm.length & 1)) throw new Error("invalid PCM16 audio");
  let peak = 0;
  let sumSquares = 0;
  const samples = pcm.length / 2;
  for (let offset = 0; offset < pcm.length; offset += 2) {
    const sample = pcm.readInt16LE(offset);
    const magnitude = Math.abs(sample);
    if (magnitude > peak) peak = magnitude;
    sumSquares += sample * sample;
  }
  const rms = Math.sqrt(sumSquares / samples);
  const dbfs = (value) => value > 0 ? 20 * Math.log10(value / 32_768) : Number.NEGATIVE_INFINITY;
  const peakRatio = peak / 32_768;
  return {
    peak,
    peakDbfs: dbfs(peak),
    rmsDbfs: dbfs(rms),
    limiterReached: peakRatio >= limiter - (2 / 32_768),
    clipped: peak >= 32_767,
    bytes: pcm.length,
  };
}

/** Replaceable backend boundary. Implementations return raw PCM in the requested format. */
export class EspeakTtsBackend {
  constructor({ voice = "en", rate = 155, maxPcmBytes = 1_920_000, gainDb = SPEAKER_GAIN_DB, limiter = SPEAKER_LIMITER } = {}) {
    this.voice = voice;
    this.rate = rate;
    this.maxPcmBytes = maxPcmBytes;
    this.gainDb = gainDb;
    this.limiter = limiter;
    this.filter = speakerAudioFilter(gainDb, limiter);
  }

  async synthesize(text, format) {
    const espeak = spawn("espeak-ng", ["--stdout", "-v", this.voice, "-s", String(this.rate), text], { stdio: ["ignore", "pipe", "pipe"] });
    // Resample before conditioning so output resampling cannot create peaks after the limiter.
    const filter = `aresample=${format.sampleRate},${this.filter}`;
    const ffmpeg = spawn("ffmpeg", ["-hide_banner", "-loglevel", "error", "-i", "pipe:0", "-af", filter, "-f", "s16le", "-acodec", "pcm_s16le", "-ac", String(format.channels), "-ar", String(format.sampleRate), "pipe:1"], { stdio: ["pipe", "pipe", "pipe"] });
    espeak.stdout.pipe(ffmpeg.stdin);
    const espeakError = new Promise((_, reject) => {
      let stderr = "";
      espeak.stderr.on("data", (chunk) => { stderr = `${stderr}${chunk}`.slice(-2_000); });
      espeak.once("error", reject);
      espeak.once("close", (code) => { if (code !== 0) reject(new Error(`espeak-ng exited ${code}: ${stderr.trim()}`)); });
    });
    const pcm = await Promise.race([collectProcessOutput(ffmpeg, this.maxPcmBytes, "ffmpeg"), espeakError]);
    if (pcm.length === 0 || pcm.length % 2 !== 0) throw new Error("TTS returned invalid PCM16 audio");
    return pcm;
  }
}

export function speakerOutstandingBytes(sentBytes, consumedBytes) {
  if (!Number.isInteger(sentBytes) || !Number.isInteger(consumedBytes) || sentBytes < 0 || consumedBytes < 0 || consumedBytes > sentBytes) {
    throw new Error("invalid speaker flow counters");
  }
  return sentBytes - consumedBytes;
}

export function speakerCreditBytes(sentBytes, consumedBytes, {
  targetOutstandingBytes = SPEAKER_TARGET_OUTSTANDING_BYTES,
  maxOutstandingBytes = SPEAKER_MAX_OUTSTANDING_BYTES,
} = {}) {
  if (!Number.isInteger(targetOutstandingBytes) || !Number.isInteger(maxOutstandingBytes) || targetOutstandingBytes <= 0 || maxOutstandingBytes < targetOutstandingBytes) {
    throw new Error("invalid speaker flow window");
  }
  const outstanding = speakerOutstandingBytes(sentBytes, consumedBytes);
  if (outstanding >= maxOutstandingBytes) return 0;
  return Math.max(0, Math.min(targetOutstandingBytes - outstanding, maxOutstandingBytes - outstanding));
}

export function createSpeakerRuntime({
  logger,
  backend,
  enabled,
  getDevice,
  sendControl,
  isPersistentEnabled = () => true,
  format = { sampleRate: 16_000, channels: 1, bitsPerSample: 16 },
  chunkBytes = 1_024,
  maxTextChars = 300,
  connectionTimeoutMs = 9_000,
  resultTimeoutMs = 75_000,
  flowTimeoutMs = SPEAKER_FLOW_TIMEOUT_MS,
  targetOutstandingBytes = SPEAKER_TARGET_OUTSTANDING_BYTES,
  maxOutstandingBytes = SPEAKER_MAX_OUTSTANDING_BYTES,
  maxPendingJobs = 4,
}) {
  if (!Number.isInteger(chunkBytes) || chunkBytes <= 0) throw new Error("invalid speaker chunk size");
  if (SPEAKER_INITIAL_LEAD_BYTES > maxOutstandingBytes || targetOutstandingBytes > maxOutstandingBytes) throw new Error("invalid speaker flow configuration");

  const jobs = new Map();
  const allJobs = new Set();
  const connectionWaiters = new Set();
  let connection = null;
  let queue = Promise.resolve();
  let closing = false;

  function closeConnection(reason = "speaker disabled") {
    const current = connection;
    if (!current) return;
    connection = null;
    if (current.ws.readyState === 1) current.ws.close(1000, reason);
  }

  function rejectFlowWaiters(job, error) {
    for (const waiter of job.flowWaiters) {
      clearTimeout(waiter.timer);
      waiter.reject(error);
    }
    job.flowWaiters.clear();
  }

  function settle(job, result) {
    if (job.settled) return;
    job.settled = true;
    clearTimeout(job.resultTimer);
    rejectFlowWaiters(job, new Error(result.error ?? result.kind));
    jobs.delete(job.id);
    allJobs.delete(job);
    if (job.temporary && !isPersistentEnabled()) closeConnection("temporary playback complete");
    result.kind === "complete" ? job.resolve(result) : job.reject(new Error(result.error ?? result.kind));
  }

  function waitForConnection(timeoutMs = connectionTimeoutMs) {
    if (connection?.ws.readyState === 1) return Promise.resolve(connection);
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject, timer: null };
      waiter.timer = setTimeout(() => {
        connectionWaiters.delete(waiter);
        reject(new Error("speaker connection timeout"));
      }, timeoutMs);
      waiter.timer.unref();
      connectionWaiters.add(waiter);
    });
  }

  function resolveConnectionWaiters(current) {
    for (const waiter of connectionWaiters) {
      clearTimeout(waiter.timer);
      waiter.resolve(current);
    }
    connectionWaiters.clear();
  }

  async function requestTemporaryConnection(job) {
    if (connection?.ws.readyState === 1) return connection;
    const device = getDevice();
    if (!device) throw new Error("speaker unavailable");
    const sent = await sendControl({ type: "speaker_control", action: "temporary_connect" }, device);
    if (!sent) throw new Error("temporary speaker connection request failed");
    logger.info({ playback_id: job.id }, "Temporary speaker connection requested");
    return waitForConnection();
  }

  function sendFrame(ws, data, options) {
    return new Promise((resolve, reject) => {
      try { ws.send(data, options, (error) => error ? reject(error) : resolve()); }
      catch (error) { reject(error); }
    });
  }

  function waitForFlow(job) {
    if (job.settled) return Promise.reject(new Error("speaker playback already settled"));
    const version = job.flowVersion;
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject, timer: null, version };
      waiter.timer = setTimeout(() => {
        job.flowWaiters.delete(waiter);
        reject(new Error("speaker flow timeout"));
      }, flowTimeoutMs);
      job.flowWaiters.add(waiter);
      if (job.flowVersion !== version) {
        clearTimeout(waiter.timer);
        job.flowWaiters.delete(waiter);
        resolve();
      }
    });
  }

  function signalFlow(job) {
    job.flowVersion += 1;
    for (const waiter of job.flowWaiters) {
      clearTimeout(waiter.timer);
      waiter.resolve();
    }
    job.flowWaiters.clear();
  }

  function handleFlow(current, message) {
    const job = jobs.get(message.playback_id);
    if (!job || connection !== current || job.settled) return false;
    const consumedBytes = message.consumed_bytes;
    const bufferedBytes = message.buffered_bytes;
    if (!Number.isInteger(consumedBytes) || !Number.isInteger(bufferedBytes) || consumedBytes < 0 || bufferedBytes < 0 ||
        consumedBytes < job.consumedBytes || consumedBytes > job.bytesSent) {
      logger.warn({ device_id: current.deviceId, playback_id: message.playback_id, consumed_bytes: consumedBytes, buffered_bytes: bufferedBytes, bytes_sent: job.bytesSent }, "Ignored invalid speaker flow report");
      return false;
    }
    if (consumedBytes === job.consumedBytes && bufferedBytes === job.reportedBufferedBytes) return true;
    job.consumedBytes = consumedBytes;
    job.reportedBufferedBytes = bufferedBytes;
    job.flowReports += 1;
    job.minReportedBufferedBytes = Math.min(job.minReportedBufferedBytes, bufferedBytes);
    job.maxReportedBufferedBytes = Math.max(job.maxReportedBufferedBytes, bufferedBytes);
    signalFlow(job);
    return true;
  }

  function handleSocketMessage(current, data, isBinary) {
    if (isBinary) return false;
    let message;
    try { message = JSON.parse(Buffer.isBuffer(data) ? data.toString("utf8") : String(data)); }
    catch { return false; }
    if (message?.type !== "speaker_flow") return false;
    return handleFlow(current, message);
  }

  function recordOutstanding(job) {
    const outstanding = speakerOutstandingBytes(job.bytesSent, job.consumedBytes);
    job.maxOutstandingBytes = Math.max(job.maxOutstandingBytes, outstanding);
    if (outstanding > maxOutstandingBytes) throw new Error("speaker flow window exceeded");
    return outstanding;
  }

  async function sendPcmChunk(job, current, chunk) {
    if (job.settled || connection !== current || current.ws.readyState !== 1) throw new Error("speaker disconnected");
    const nextBytesSent = job.bytesSent + chunk.length;
    const nextOutstanding = nextBytesSent - job.consumedBytes;
    if (nextOutstanding > maxOutstandingBytes) throw new Error("speaker flow window exceeded");
    job.bytesSent = nextBytesSent;
    await sendFrame(current.ws, chunk, { binary: true });
    recordOutstanding(job);
    if (job.firstPcmSentAt === null) {
      job.firstPcmSentAt = performance.now();
      logger.info({ playback_id: job.id, ready_to_first_pcm_ms: Math.round(job.firstPcmSentAt - job.readyAt) }, "Speaker first PCM sent");
    }
  }

  async function stream(job, current) {
    if (connection !== current || current.ws.readyState !== 1) throw new Error("speaker disconnected");
    const ws = current.ws;
    jobs.set(job.id, job);
    job.resultTimer = setTimeout(() => settle(job, { kind: "timeout", error: "speaker playback timeout" }), resultTimeoutMs);
    job.resultTimer.unref();
    const begin = { type: "speaker_begin", playback_id: job.id, sample_rate: format.sampleRate, channels: format.channels, bits_per_sample: format.bitsPerSample, bytes: job.pcm.length };
    job.beginSentAt = performance.now();
    await sendFrame(ws, JSON.stringify(begin));
    logger.info({ device_id: current.deviceId, playback_id: job.id, pcm_bytes: job.pcm.length }, "Speaker begin sent");

    const streamStartedAt = performance.now();
    const leadBytes = Math.min(SPEAKER_INITIAL_LEAD_BYTES, job.pcm.length);
    while (job.bytesSent < leadBytes) {
      const end = Math.min(job.bytesSent + chunkBytes, leadBytes);
      await sendPcmChunk(job, current, job.pcm.subarray(job.bytesSent, end));
    }

    while (job.bytesSent < job.pcm.length) {
      if (job.settled || connection !== current || ws.readyState !== 1) throw new Error("speaker disconnected");
      const nextLength = Math.min(chunkBytes, job.pcm.length - job.bytesSent);
      const credit = speakerCreditBytes(job.bytesSent, job.consumedBytes, { targetOutstandingBytes, maxOutstandingBytes });
      if (credit < nextLength) {
        await waitForFlow(job);
        continue;
      }
      const start = job.bytesSent;
      await sendPcmChunk(job, current, job.pcm.subarray(start, start + nextLength));
    }

    await sendFrame(ws, JSON.stringify({ type: "speaker_end", playback_id: job.id, bytes: job.pcm.length }));
    logger.info({
      device_id: current.deviceId,
      playback_id: job.id,
      bytes: job.pcm.length,
      stream_ms: Math.round(performance.now() - streamStartedAt),
      pacing: "receiver_credit",
      lead_bytes: leadBytes,
      target_outstanding_bytes: targetOutstandingBytes,
      max_outstanding_limit_bytes: maxOutstandingBytes,
      max_outstanding_bytes: job.maxOutstandingBytes,
      flow_reports: job.flowReports,
      min_reported_buffer: Number.isFinite(job.minReportedBufferedBytes) ? job.minReportedBufferedBytes : null,
      max_reported_buffer: job.maxReportedBufferedBytes,
      chunk_bytes: chunkBytes,
    }, "Speaker PCM stream sent");
    return job.completion;
  }

  async function run(job) {
    if (job.settled || closing) return;
    if (!getDevice()) throw new Error("speaker unavailable");
    logger.info({ playback_id: job.id, text_chars: job.text.length }, "Speaker TTS synthesis started");
    const connectionPromise = job.temporary ? requestTemporaryConnection(job) : waitForConnection();
    connectionPromise.catch(() => {});
    const synthesisStartedAt = performance.now();
    job.synthesisStartedAt = synthesisStartedAt;
    const pcm = await backend.synthesize(job.text, format);
    job.ttsCompletedAt = performance.now();
    job.pcm = pcm;
    const audio = analyzePcm16(pcm, backend.limiter ?? SPEAKER_LIMITER);
    logger.info({
      event: "SPEAKER_AUDIO", playback_id: job.id,
      peak_dbfs: Number.isFinite(audio.peakDbfs) ? Number(audio.peakDbfs.toFixed(1)) : "-inf",
      rms_dbfs: Number.isFinite(audio.rmsDbfs) ? Number(audio.rmsDbfs.toFixed(1)) : "-inf",
      gain_db: backend.gainDb ?? SPEAKER_GAIN_DB,
      limiter: backend.limiter ?? SPEAKER_LIMITER,
      limiter_reached: audio.limiterReached,
      clipped: audio.clipped,
      bytes: pcm.length,
    }, "SPEAKER_AUDIO");
    logger.info({ playback_id: job.id, pcm_bytes: pcm.length, tts_ms: Math.round(job.ttsCompletedAt - synthesisStartedAt), format }, "Speaker TTS synthesis completed");
    const current = await connectionPromise;
    job.readyAt = performance.now();
    return stream(job, current);
  }

  function speak(html, { maxChars = maxTextChars, temporary = false } = {}) {
    if (!enabled || closing) return { kind: "disabled" };
    if (allJobs.size >= maxPendingJobs) return { kind: "busy" };
    const text = telegramHtmlToSpeech(html, maxChars);
    if (!text) return { kind: "empty" };
    if (!getDevice()) return { kind: "offline" };
    const id = randomUUID();
    let resolve;
    let reject;
    const completion = new Promise((res, rej) => { resolve = res; reject = rej; });
    completion.catch(() => {});
    const job = {
      id, text, pcm: null, temporary, settled: false, resultTimer: null,
      queuedAt: performance.now(), synthesisStartedAt: null, ttsCompletedAt: null, readyAt: null,
      beginSentAt: null, firstPcmSentAt: null, resolve, reject, completion,
      bytesSent: 0, consumedBytes: 0, reportedBufferedBytes: 0, flowVersion: 0, flowWaiters: new Set(),
      flowReports: 0, maxOutstandingBytes: 0, minReportedBufferedBytes: Number.POSITIVE_INFINITY, maxReportedBufferedBytes: 0,
    };
    allJobs.add(job);
    logger.info({ playback_id: id, text_chars: text.length, temporary }, "Speaker job queued");
    queue = queue.catch(() => {}).then(() => run(job)).catch((error) => {
      settle(job, { kind: "error", error: error?.message ?? "TTS failed" });
      logger.warn({ playback_id: id, error_message: error?.message ?? "unknown" }, "Speaker/TTS job failed");
    });
    return { kind: "queued", playbackId: id, text, completion };
  }

  function handleConnection(ws, deviceId) {
    if (connection?.ws && connection.ws !== ws && connection.ws.readyState === 1) connection.ws.close(4001, "replaced by new connection");
    const current = { ws, deviceId, connectedAt: performance.now() };
    connection = current;
    ws.on?.("message", (data, isBinary) => { handleSocketMessage(current, data, isBinary); });
    ws.on?.("close", () => {
      if (connection === current) connection = null;
      for (const job of jobs.values()) rejectFlowWaiters(job, new Error("speaker disconnected"));
      logger.info({ device_id: deviceId }, "Persistent speaker stream disconnected");
    });
    ws.on?.("error", () => logger.warn({ device_id: deviceId }, "Persistent speaker WebSocket error"));
    resolveConnectionWaiters(current);
    logger.info({ device_id: deviceId }, "Persistent speaker stream ready");
  }

  async function handleDeviceConnected(deviceId, device) {
    for (const job of allJobs) {
      if (!job.temporary || job.settled || connection?.ws.readyState === 1) continue;
      try { await sendControl({ type: "speaker_control", action: "temporary_connect" }, device); }
      catch (error) { logger.warn({ device_id: deviceId, playback_id: job.id, error_message: error?.message ?? "unknown" }, "Temporary speaker reconnect request failed"); }
    }
  }

  function handlePlaybackStarted(deviceId, message) {
    const job = jobs.get(message.playback_id);
    if (!job || job.firstPcmSentAt === null) return false;
    const firstPcmToPlayMs = message.first_pcm_to_play_ms;
    const metrics = {
      device_id: deviceId,
      playback_id: job.id,
      tts_ms: Math.round(job.ttsCompletedAt - job.synthesisStartedAt),
      ready_to_first_pcm_ms: Math.round(job.firstPcmSentAt - job.readyAt),
      first_pcm_to_play_ms: firstPcmToPlayMs,
      total_start_latency_ms: Math.round(job.firstPcmSentAt - job.queuedAt + firstPcmToPlayMs),
    };
    logger.info(metrics, "SPEAKER_LATENCY");
    return true;
  }

  function handleResult(deviceId, message) {
    const job = jobs.get(message.playback_id);
    if (!job) return false;
    const result = message.type === "speaker_complete" ? { kind: "complete", bytes: message.bytes } : { kind: "error", error: message.error ?? "device playback failed" };
    settle(job, result);
    logger[message.type === "speaker_complete" ? "info" : "warn"]({ device_id: deviceId, playback_id: message.playback_id, bytes: message.bytes, error: message.error }, `Speaker playback ${message.type === "speaker_complete" ? "complete" : "failed"}`);
    return true;
  }

  function setPersistentEnabled(persistentEnabled) {
    if (persistentEnabled) return;
    for (const job of [...allJobs]) {
      if (!job.temporary) settle(job, { kind: "disabled", error: "speaker disabled" });
    }
  }

  function close() {
    closing = true;
    for (const waiter of connectionWaiters) { clearTimeout(waiter.timer); waiter.reject(new Error("server shutting down")); }
    connectionWaiters.clear();
    for (const job of [...allJobs]) settle(job, { kind: "shutdown", error: "server shutting down" });
    closeConnection("server shutting down");
  }

  return { speak, handleConnection, handleDeviceConnected, handlePlaybackStarted, handleResult, setPersistentEnabled, close, isReady: () => connection?.ws.readyState === 1, format };
}
