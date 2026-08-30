import { once } from "node:events";
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

export const SPEAKER_GAIN_DB = 2;
export const ESPEAK_GAIN_DB = 6;
export const SPEAKER_LIMITER = 0.95;
export const SPEAKER_LIMITER_ATTACK_MS = 5;
export const SPEAKER_RECEIVER_CAPACITY_BYTES = 24_576;
export const SPEAKER_RECEIVER_BUFFER_TARGET_BYTES = 22_528;
export const SPEAKER_RECEIVER_LOW_WATER_BYTES = 10_240;
export const SPEAKER_NETWORK_INFLIGHT_LIMIT_BYTES = 22_528;
export const SPEAKER_MAX_OUTSTANDING_BYTES = 22_528;
export const SPEAKER_FLOW_TIMEOUT_MS = 3_000;
export const SPEAKER_STREAM_MAX_BYTES = 2_880_000;
export const SPEAKER_STREAM_NO_PROGRESS_MS = 10_000;
export const SPEAKER_STREAM_ABSOLUTE_MS = 70_000;

export function speakerAudioFilter(gainDb = SPEAKER_GAIN_DB, limiter = SPEAKER_LIMITER) {
  if (!Number.isFinite(gainDb) || gainDb < -12 || gainDb > 18) throw new Error("invalid speaker gain");
  if (!Number.isFinite(limiter) || limiter <= 0 || limiter > 1) throw new Error("invalid speaker limiter");
  return `highpass=f=110,volume=${gainDb}dB:precision=float,alimiter=limit=${limiter}:attack=${SPEAKER_LIMITER_ATTACK_MS}:release=50:level=false:latency=true`;
}

export const SPEAKER_AUDIO_FILTER = speakerAudioFilter();

function pcmStatistics() {
  let peak = 0;
  let sumSquares = 0;
  let samples = 0;
  return {
    add(pcm) {
      if (!Buffer.isBuffer(pcm) || (pcm.length & 1)) throw new Error("invalid PCM16 audio");
      for (let offset = 0; offset < pcm.length; offset += 2) {
        const sample = pcm.readInt16LE(offset);
        peak = Math.max(peak, Math.abs(sample));
        sumSquares += sample * sample;
        samples += 1;
      }
    },
    result(limiter = SPEAKER_LIMITER) {
      if (samples === 0) throw new Error("invalid PCM16 audio");
      const dbfs = (value) => value > 0 ? 20 * Math.log10(value / 32_768) : Number.NEGATIVE_INFINITY;
      return {
        peak, peakDbfs: dbfs(peak), rmsDbfs: dbfs(Math.sqrt(sumSquares / samples)),
        limiterReached: peak / 32_768 >= limiter - (2 / 32_768), clipped: peak >= 32_767,
        bytes: samples * 2,
      };
    },
  };
}

export function analyzePcm16(pcm, limiter = SPEAKER_LIMITER) {
  if (!Buffer.isBuffer(pcm) || pcm.length === 0 || (pcm.length & 1)) throw new Error("invalid PCM16 audio");
  const statistics = pcmStatistics();
  statistics.add(pcm);
  return statistics.result(limiter);
}

export async function conditionPcm16(pcm, format, {
  gainDb = SPEAKER_GAIN_DB, limiter = SPEAKER_LIMITER, maxPcmBytes = SPEAKER_STREAM_MAX_BYTES,
} = {}) {
  if (!Buffer.isBuffer(pcm) || pcm.length === 0 || (pcm.length & 1)) throw new Error("invalid PCM16 audio");
  validateFormat(format);
  const ffmpeg = createConditioner(format, gainDb, limiter);
  ffmpeg.stdin.end(pcm);
  const conditioned = await collectProcessOutput(ffmpeg, maxPcmBytes, "ffmpeg");
  if (conditioned.length === 0 || (conditioned.length & 1)) throw new Error("ffmpeg returned invalid PCM16 audio");
  return conditioned;
}

function validateFormat(format) {
  if (format?.sampleRate !== 24_000 || format?.channels !== 1 || format?.bitsPerSample !== 16) {
    throw new Error("Kokoro requires mono 24000 Hz PCM16 output");
  }
}

function createConditioner(format, gainDb, limiter) {
  return spawn("ffmpeg", [
    "-hide_banner", "-loglevel", "error", "-fflags", "nobuffer", "-flags", "low_delay",
    "-probesize", "32", "-analyzeduration", "0", "-f", "s16le", "-ac", "1", "-ar", String(format.sampleRate),
    "-i", "pipe:0", "-af", speakerAudioFilter(gainDb, limiter), "-flush_packets", "1",
    "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", "-ar", String(format.sampleRate), "pipe:1",
  ], { stdio: ["pipe", "pipe", "pipe"] });
}

async function collectResponseBody(response, maxBytes, label) {
  const chunks = [];
  let bytes = 0;
  for await (const chunk of response.body ?? []) {
    const buffer = Buffer.from(chunk);
    bytes += buffer.length;
    if (bytes > maxBytes) {
      await response.body?.cancel?.().catch(() => {});
      throw new Error(`${label} output exceeded limit`);
    }
    chunks.push(buffer);
  }
  return Buffer.concat(chunks);
}

function timeoutError(code) {
  const error = new Error(code);
  error.code = code;
  return error;
}

async function readWithTimeout(reader, timeoutMs, controller) {
  let timer;
  try {
    return await Promise.race([
      reader.read(),
      new Promise((_, reject) => {
        timer = setTimeout(() => {
          const error = timeoutError("kokoro_stream_timeout");
          controller.abort(error);
          reject(error);
        }, timeoutMs);
        timer.unref();
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

export function splitRealtimeText(text, { firstSegmentTargetChars = 28, minimumOpeningChars = 14, maximumSegmentChars = 90 } = {}) {
  const input = String(text).trim();
  if (!input) return [];
  if (!Number.isInteger(firstSegmentTargetChars) || firstSegmentTargetChars < minimumOpeningChars ||
      !Number.isInteger(minimumOpeningChars) || minimumOpeningChars < 12 ||
      !Number.isInteger(maximumSegmentChars) || maximumSegmentChars < firstSegmentTargetChars) {
    throw new Error("invalid realtime segmentation policy");
  }
  const cutAtBoundary = (value, target) => {
    if (value.length <= target) return value.length;
    const prefix = value.slice(0, target + 1);
    for (const boundary of [/[.!?][\"']?(?=\s|$)/g, /[,;:](?=\s|$)/g, /\s/g]) {
      const matches = [...prefix.matchAll(boundary)];
      if (matches.length) return matches.at(-1).index + matches.at(-1)[0].length;
    }
    const nextSpace = value.slice(target).search(/\s/);
    return nextSpace < 0 ? Math.min(target, value.length) : target + nextSpace;
  };
  const bounded = (value, maximum) => {
    const parts = [];
    let rest = value.trim();
    while (rest.length > maximum) {
      const cut = cutAtBoundary(rest, maximum);
      parts.push(rest.slice(0, cut).trim());
      rest = rest.slice(cut).trim();
    }
    if (rest) parts.push(rest);
    return parts;
  };
  const segments = [];
  let remainder = input;
  let firstCut = cutAtBoundary(remainder, firstSegmentTargetChars);
  // Avoid isolated acknowledgements such as "Yes." while still starting early.
  if (firstCut < minimumOpeningChars && remainder.length > minimumOpeningChars) {
    const openingWords = [...remainder.slice(0, firstSegmentTargetChars + 1).matchAll(/\s/g)];
    const lastWordBoundary = openingWords.at(-1);
    firstCut = lastWordBoundary ? lastWordBoundary.index : cutAtBoundary(remainder, firstSegmentTargetChars);
  }
  if (firstCut < remainder.length) { segments.push(remainder.slice(0, firstCut).trim()); remainder = remainder.slice(firstCut).trim(); }
  const sentences = remainder.match(/[^.!?]+[.!?]+(?:[\"'](?=\s|$))?|[^.!?]+$/g)?.map((part) => part.trim()).filter(Boolean) ?? [remainder];
  let current = "";
  for (const sentence of sentences.flatMap((part) => bounded(part, maximumSegmentChars))) {
    const candidate = current ? `${current} ${sentence}` : sentence;
    if (current && candidate.length > maximumSegmentChars) { segments.push(current); current = sentence; }
    else current = candidate;
  }
  if (current) segments.push(current);
  return segments;
}

/** Local OpenTTSGroup Kokoro realtime backend. Upstream PCM is raw mono 24 kHz s16le. */
export class KokoroTtsBackend {
  constructor({
    baseUrl = "http://127.0.0.1:8010", voice = "am_michael", speed = 1,
    requestTimeoutMs = 30_000, streamNoProgressMs = SPEAKER_STREAM_NO_PROGRESS_MS,
    streamAbsoluteMs = SPEAKER_STREAM_ABSOLUTE_MS, maxPcmBytes = SPEAKER_STREAM_MAX_BYTES,
    gainDb = SPEAKER_GAIN_DB, limiter = SPEAKER_LIMITER, logger = null,
  } = {}) {
    if (!Number.isFinite(speed) || speed < 0.25 || speed > 4) throw new Error("invalid Kokoro speed");
    this.baseUrl = String(baseUrl).replace(/\/+$/, "");
    this.voice = voice;
    this.speed = speed;
    this.requestTimeoutMs = requestTimeoutMs;
    this.streamNoProgressMs = streamNoProgressMs;
    this.streamAbsoluteMs = streamAbsoluteMs;
    this.maxPcmBytes = maxPcmBytes;
    this.gainDb = gainDb;
    this.limiter = limiter;
    this.logger = logger;
  }

  async streamSegment(text, format, { signal } = {}) {
    validateFormat(format);
    const controller = new AbortController();
    const cancelFromParent = () => controller.abort(signal?.reason ?? timeoutError("kokoro_stream_cancelled"));
    if (signal?.aborted) cancelFromParent();
    else signal?.addEventListener("abort", cancelFromParent, { once: true });
    const removeParentSignal = () => signal?.removeEventListener("abort", cancelFromParent);
    const metrics = {
      requestStartedAt: performance.now(), responseHeadersAt: null, firstAudioByteAt: null,
      conditionerFirstOutputAt: null, completedAt: null, rawPcmBytes: 0, conditionedPcmBytes: 0,
    };
    const absoluteTimer = setTimeout(() => controller.abort(timeoutError("kokoro_stream_timeout")), this.streamAbsoluteMs);
    const requestTimer = setTimeout(() => controller.abort(timeoutError("kokoro_request_timeout")), this.requestTimeoutMs);
    absoluteTimer.unref();
    requestTimer.unref();
    let response;
    try {
      response = await fetch(`${this.baseUrl}/v1/audio/realtime`, {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/octet-stream" },
        body: JSON.stringify({ model: "kokoro", input: text, voice: this.voice, response_format: "pcm", speed: this.speed }),
        signal: controller.signal,
      });
    } catch (error) {
      clearTimeout(requestTimer);
      clearTimeout(absoluteTimer);
      removeParentSignal();
      if (controller.signal.aborted || error?.name === "TimeoutError" || error?.name === "AbortError") {
        throw timeoutError(controller.signal.reason?.code ?? "kokoro_request_timeout");
      }
      throw new Error(`Kokoro unavailable: ${error?.message ?? "request failed"}`);
    }
    clearTimeout(requestTimer);
    metrics.responseHeadersAt = performance.now();
    if (!response.ok) {
      clearTimeout(absoluteTimer);
      removeParentSignal();
      const detail = (await collectResponseBody(response, 2_000, "Kokoro error")).toString("utf8").trim();
      throw new Error(`Kokoro request failed (${response.status})${detail ? `: ${detail}` : ""}`);
    }
    const contentType = response.headers.get("content-type")?.split(";", 1)[0].trim().toLowerCase();
    if (contentType !== "application/octet-stream") {
      clearTimeout(absoluteTimer);
      removeParentSignal();
      await response.body?.cancel?.().catch(() => {});
      throw new Error(`Kokoro returned non-PCM content type: ${contentType ?? "missing"}`);
    }
    if (!response.body) {
      clearTimeout(absoluteTimer);
      removeParentSignal();
      throw new Error("Kokoro returned no PCM body");
    }

    const backend = this;
    async function* conditionedAudio() {
      const ffmpeg = createConditioner(format, backend.gainDb, backend.limiter);
      let stderr = "";
      let inputCarry = Buffer.alloc(0);
      let outputCarry = Buffer.alloc(0);
      let pumpError = null;
      ffmpeg.stdin.on("error", () => {});
      ffmpeg.stderr.on("data", (chunk) => { stderr = `${stderr}${chunk}`.slice(-2_000); });
      const closePromise = once(ffmpeg, "close").then(([code]) => {
        if (code !== 0 && !pumpError) throw new Error(`ffmpeg exited ${code}: ${stderr.trim()}`);
      });
      const reader = response.body.getReader();
      const pump = (async () => {
        try {
          while (true) {
            const { done, value } = await readWithTimeout(reader, backend.streamNoProgressMs, controller);
            if (done) break;
            let chunk = Buffer.from(value);
            if (metrics.firstAudioByteAt === null) metrics.firstAudioByteAt = performance.now();
            metrics.rawPcmBytes += chunk.length;
            if (metrics.rawPcmBytes > backend.maxPcmBytes) throw new Error("Kokoro output exceeded limit");
            if (inputCarry.length) { chunk = Buffer.concat([inputCarry, chunk]); inputCarry = Buffer.alloc(0); }
            if (chunk.length & 1) { inputCarry = chunk.subarray(chunk.length - 1); chunk = chunk.subarray(0, chunk.length - 1); }
            if (chunk.length && !ffmpeg.stdin.write(chunk)) await once(ffmpeg.stdin, "drain");
          }
          if (inputCarry.length) throw new Error("invalid_pcm");
          ffmpeg.stdin.end();
        } catch (error) {
          pumpError = controller.signal.aborted
            ? timeoutError(controller.signal.reason?.code ?? "kokoro_stream_timeout")
            : error;
          ffmpeg.kill("SIGKILL");
          throw pumpError;
        }
      })();
      pump.catch(() => {});
      try {
        for await (const value of ffmpeg.stdout) {
          let chunk = Buffer.from(value);
          if (outputCarry.length) { chunk = Buffer.concat([outputCarry, chunk]); outputCarry = Buffer.alloc(0); }
          if (chunk.length & 1) { outputCarry = chunk.subarray(chunk.length - 1); chunk = chunk.subarray(0, chunk.length - 1); }
          if (!chunk.length) continue;
          if (metrics.conditionerFirstOutputAt === null) metrics.conditionerFirstOutputAt = performance.now();
          metrics.conditionedPcmBytes += chunk.length;
          if (metrics.conditionedPcmBytes > backend.maxPcmBytes) throw new Error("conditioned PCM exceeded limit");
          yield chunk;
        }
        await pump;
        await closePromise;
        if (outputCarry.length || metrics.conditionedPcmBytes === 0) throw new Error("invalid_pcm");
        metrics.completedAt = performance.now();
        backend.logger?.info({
          voice: backend.voice, speed: backend.speed, raw_pcm_bytes: metrics.rawPcmBytes,
          pcm_bytes: metrics.conditionedPcmBytes,
          first_audio_byte_ms: Math.round(metrics.firstAudioByteAt - metrics.requestStartedAt),
          conditioner_first_output_ms: Math.round(metrics.conditionerFirstOutputAt - metrics.firstAudioByteAt),
          full_generation_ms: Math.round(metrics.completedAt - metrics.requestStartedAt),
        }, "Kokoro realtime synthesis completed");
      } finally {
        clearTimeout(absoluteTimer);
        removeParentSignal();
        if (!controller.signal.aborted) controller.abort(timeoutError("kokoro_stream_cancelled"));
        await reader.cancel().catch(() => {});
        if (ffmpeg.exitCode === null) ffmpeg.kill("SIGKILL");
        await Promise.allSettled([pump, closePromise]);
        metrics.cancelled = metrics.completedAt === null;
        reader.releaseLock();
      }
    }
    return { audio: conditionedAudio(), metrics, streaming: true };
  }

  async stream(text, format) {
    validateFormat(format);
    const segments = splitRealtimeText(text);
    const metrics = {
      requestStartedAt: performance.now(), responseHeadersAt: null, firstAudioByteAt: null,
      conditionerFirstOutputAt: null, completedAt: null, rawPcmBytes: 0, conditionedPcmBytes: 0,
      segments: segments.length, producerQueuedBytes: 0, producerQueueHighWaterBytes: 0,
    };
    const queued = [];
    const waiters = [];
    let producerError = null;
    let producerDone = false;
    let cancelled = false;
    const producerController = new AbortController();
    const wake = () => { while (waiters.length) waiters.shift()(); };
    const producer = (async () => {
      try {
        for (const segment of segments) {
          if (cancelled) break;
          const source = await this.streamSegment(segment, format, { signal: producerController.signal });
          try {
            for await (const chunk of source.audio) {
              if (cancelled) break;
              if (metrics.responseHeadersAt === null) metrics.responseHeadersAt = source.metrics.responseHeadersAt;
              if (metrics.firstAudioByteAt === null) metrics.firstAudioByteAt = source.metrics.firstAudioByteAt;
              if (metrics.conditionerFirstOutputAt === null) metrics.conditionerFirstOutputAt = source.metrics.conditionerFirstOutputAt;
              metrics.conditionedPcmBytes += chunk.length;
              if (metrics.conditionedPcmBytes > this.maxPcmBytes) throw new Error("Kokoro output exceeded limit");
              queued.push(chunk);
              metrics.producerQueuedBytes += chunk.length;
              metrics.producerQueueHighWaterBytes = Math.max(metrics.producerQueueHighWaterBytes, metrics.producerQueuedBytes);
              wake();
            }
          } finally {
            metrics.rawPcmBytes += source.metrics.rawPcmBytes;
          }
        }
        metrics.completedAt = performance.now();
      } catch (error) {
        if (!cancelled) producerError = error;
      } finally {
        producerDone = true;
        wake();
      }
    })();
    producer.catch(() => {});
    function cancel() {
      if (cancelled) return;
      cancelled = true;
      producerController.abort(timeoutError("kokoro_stream_cancelled"));
      queued.length = 0;
      metrics.producerQueuedBytes = 0;
      wake();
    }
    async function* audio() {
      try {
        while (!producerDone || queued.length) {
          if (!queued.length) await new Promise((resolve) => waiters.push(resolve));
          while (queued.length) {
            const chunk = queued.shift();
            metrics.producerQueuedBytes -= chunk.length;
            yield chunk;
          }
          if (producerError) throw producerError;
        }
        if (producerError) throw producerError;
      } finally {
        cancel();
        await producer;
      }
    }
    return { audio: audio(), metrics, streaming: true, cancel };
  }

  async synthesize(text, format) {
    const source = await this.stream(text, format);
    const chunks = [];
    for await (const chunk of source.audio) chunks.push(chunk);
    return Buffer.concat(chunks);
  }
}

/** Explicit local fallback. It retains known-length framing. */
export class EspeakTtsBackend {
  constructor({ voice = "en", rate = 155, maxPcmBytes = SPEAKER_STREAM_MAX_BYTES, gainDb = ESPEAK_GAIN_DB, limiter = SPEAKER_LIMITER } = {}) {
    this.voice = voice;
    this.rate = rate;
    this.maxPcmBytes = maxPcmBytes;
    this.gainDb = gainDb;
    this.limiter = limiter;
    this.filter = speakerAudioFilter(gainDb, limiter);
  }

  async synthesize(text, format) {
    const espeak = spawn("espeak-ng", ["--stdout", "-v", this.voice, "-s", String(this.rate), text], { stdio: ["ignore", "pipe", "pipe"] });
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

export function speakerDeliveryState(sentBytes, receivedBytes, consumedBytes, bufferedBytes) {
  for (const value of [sentBytes, receivedBytes, consumedBytes, bufferedBytes]) {
    if (!Number.isInteger(value) || value < 0) throw new Error("invalid speaker delivery counters");
  }
  if (consumedBytes > receivedBytes || receivedBytes > sentBytes) throw new Error("invalid speaker delivery counters");
  return {
    networkInFlightBytes: sentBytes - receivedBytes,
    receiverOutstandingBytes: sentBytes - consumedBytes,
    committedToReceiverBytes: bufferedBytes + sentBytes - receivedBytes,
  };
}

export function speakerCreditBytes(sentBytes, receivedBytes, consumedBytes, bufferedBytes, {
  receiverBufferTargetBytes = SPEAKER_RECEIVER_BUFFER_TARGET_BYTES,
  networkInFlightLimitBytes = SPEAKER_NETWORK_INFLIGHT_LIMIT_BYTES,
  maxOutstandingBytes = SPEAKER_MAX_OUTSTANDING_BYTES,
} = {}) {
  if (!Number.isInteger(receiverBufferTargetBytes) || !Number.isInteger(networkInFlightLimitBytes) ||
      !Number.isInteger(maxOutstandingBytes) || receiverBufferTargetBytes <= 0 || networkInFlightLimitBytes <= 0 ||
      maxOutstandingBytes < receiverBufferTargetBytes) throw new Error("invalid speaker flow window");
  const state = speakerDeliveryState(sentBytes, receivedBytes, consumedBytes, bufferedBytes);
  return Math.max(0, Math.min(
    receiverBufferTargetBytes - state.committedToReceiverBytes,
    networkInFlightLimitBytes - state.networkInFlightBytes,
    maxOutstandingBytes - state.receiverOutstandingBytes,
  ));
}

export function startTelegramAndSpeech(sendTelegram, startSpeech) {
  const telegramReply = sendTelegram();
  startSpeech();
  return telegramReply;
}

export function createSpeakerRuntime({
  logger, backend, enabled, getDevice, sendControl, isPersistentEnabled = () => true,
  format = { sampleRate: 24_000, channels: 1, bitsPerSample: 16 }, chunkBytes = 2_048,
  maxTextChars = 300, maxStreamBytes = SPEAKER_STREAM_MAX_BYTES, connectionTimeoutMs = 9_000,
  resultTimeoutMs = 75_000, flowTimeoutMs = SPEAKER_FLOW_TIMEOUT_MS,
  receiverCapacityBytes = SPEAKER_RECEIVER_CAPACITY_BYTES,
  receiverBufferTargetBytes = SPEAKER_RECEIVER_BUFFER_TARGET_BYTES,
  networkInFlightLimitBytes = SPEAKER_NETWORK_INFLIGHT_LIMIT_BYTES,
  maxOutstandingBytes = SPEAKER_MAX_OUTSTANDING_BYTES, maxPendingJobs = 4,
}) {
  if (!Number.isInteger(chunkBytes) || chunkBytes <= 0 || (chunkBytes & 1)) throw new Error("invalid speaker chunk size");
  if (receiverBufferTargetBytes > receiverCapacityBytes || networkInFlightLimitBytes >= receiverCapacityBytes ||
      receiverBufferTargetBytes > maxOutstandingBytes) throw new Error("invalid speaker flow configuration");

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
    for (const waiter of job.flowWaiters) { clearTimeout(waiter.timer); waiter.reject(error); }
    job.flowWaiters.clear();
  }
  function settle(job, result) {
    if (job.settled) return;
    job.settled = true;
    clearTimeout(job.resultTimer);
    rejectFlowWaiters(job, new Error(result.error ?? result.kind));
    jobs.delete(job.id);
    allJobs.delete(job);
    if (result.kind !== "complete") void job.cancelSource?.();
    if (job.temporary && !isPersistentEnabled()) closeConnection("temporary playback complete");
    result.kind === "complete" ? job.resolve(result) : job.reject(new Error(result.error ?? result.kind));
  }
  function waitForConnection(timeoutMs = connectionTimeoutMs) {
    if (connection?.ws.readyState === 1) return Promise.resolve(connection);
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject, timer: null };
      waiter.timer = setTimeout(() => { connectionWaiters.delete(waiter); reject(new Error("speaker connection timeout")); }, timeoutMs);
      waiter.timer.unref();
      connectionWaiters.add(waiter);
    });
  }
  function resolveConnectionWaiters(current) {
    for (const waiter of connectionWaiters) { clearTimeout(waiter.timer); waiter.resolve(current); }
    connectionWaiters.clear();
  }
  async function requestTemporaryConnection(job) {
    if (connection?.ws.readyState === 1) return connection;
    const device = getDevice();
    if (!device) throw new Error("speaker unavailable");
    if (!await sendControl({ type: "speaker_control", action: "temporary_connect" }, device)) throw new Error("temporary speaker connection request failed");
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
      waiter.timer = setTimeout(() => { job.flowWaiters.delete(waiter); reject(timeoutError("flow_timeout")); }, flowTimeoutMs);
      waiter.timer.unref();
      job.flowWaiters.add(waiter);
      if (job.flowVersion !== version) { clearTimeout(waiter.timer); job.flowWaiters.delete(waiter); resolve(); }
    });
  }
  function signalFlow(job) {
    job.flowVersion += 1;
    for (const waiter of job.flowWaiters) { clearTimeout(waiter.timer); waiter.resolve(); }
    job.flowWaiters.clear();
  }
  function handleFlow(current, message) {
    const job = jobs.get(message.playback_id);
    if (!job || connection !== current || job.settled) return false;
    const receivedBytes = message.received_bytes;
    const consumedBytes = message.consumed_bytes;
    const bufferedBytes = message.buffered_bytes;
    const capacityBytes = message.capacity_bytes;
    if (!Number.isInteger(receivedBytes) || !Number.isInteger(consumedBytes) || !Number.isInteger(bufferedBytes) ||
        !Number.isInteger(capacityBytes) || receivedBytes < job.receivedBytes || consumedBytes < job.consumedBytes ||
        receivedBytes > job.bytesSent || consumedBytes > receivedBytes || bufferedBytes < 0 ||
        bufferedBytes > receiverCapacityBytes || capacityBytes !== receiverCapacityBytes) {
      logger.warn({
        device_id: current.deviceId, playback_id: message.playback_id, received_bytes: receivedBytes,
        consumed_bytes: consumedBytes, buffered_bytes: bufferedBytes, capacity_bytes: capacityBytes,
        bytes_sent: job.bytesSent,
      }, "Ignored invalid speaker flow report");
      return false;
    }
    if (receivedBytes === job.receivedBytes && consumedBytes === job.consumedBytes && bufferedBytes === job.reportedBufferedBytes) return true;
    job.receivedBytes = receivedBytes;
    job.consumedBytes = consumedBytes;
    job.reportedBufferedBytes = bufferedBytes;
    job.flowReports += 1;
    if (receivedBytes > job.lastFlowReceivedBytes) job.receivedFlowReports += 1;
    job.lastFlowReceivedBytes = receivedBytes;
    const activeReceiver = consumedBytes > 0 && (!job.endSent || consumedBytes < receivedBytes);
    if (activeReceiver) job.minReportedBufferedBytes = Math.min(job.minReportedBufferedBytes, bufferedBytes);
    job.maxReportedBufferedBytes = Math.max(job.maxReportedBufferedBytes, bufferedBytes);
    const state = speakerDeliveryState(job.bytesSent, receivedBytes, consumedBytes, bufferedBytes);
    job.maxNetworkInFlightBytes = Math.max(job.maxNetworkInFlightBytes, state.networkInFlightBytes);
    job.totalOutstandingHighWaterBytes = Math.max(job.totalOutstandingHighWaterBytes, state.receiverOutstandingBytes);
    signalFlow(job);
    return true;
  }
  function handleSocketMessage(current, data, isBinary) {
    if (isBinary) return false;
    let message;
    try { message = JSON.parse(Buffer.isBuffer(data) ? data.toString("utf8") : String(data)); }
    catch { return false; }
    return message?.type === "speaker_flow" ? handleFlow(current, message) : false;
  }
  function recordOutstanding(job) {
    const state = speakerDeliveryState(job.bytesSent, job.receivedBytes, job.consumedBytes, job.reportedBufferedBytes);
    job.maxNetworkInFlightBytes = Math.max(job.maxNetworkInFlightBytes, state.networkInFlightBytes);
    job.totalOutstandingHighWaterBytes = Math.max(job.totalOutstandingHighWaterBytes, state.receiverOutstandingBytes);
    if (state.networkInFlightBytes > networkInFlightLimitBytes || state.receiverOutstandingBytes > maxOutstandingBytes) {
      throw new Error("speaker flow window exceeded");
    }
  }
  async function sendPcmChunk(job, current, chunk) {
    if (!chunk.length || (chunk.length & 1)) throw new Error("invalid_pcm");
    if (job.settled || connection !== current || current.ws.readyState !== 1) throw new Error("speaker disconnected");
    if (job.bytesSent + chunk.length > maxStreamBytes) throw new Error("speaker stream exceeded limit");
    if (job.bytesSent + chunk.length - job.consumedBytes > maxOutstandingBytes) throw new Error("speaker flow window exceeded");
    // Account before awaiting the socket callback so an exceptionally fast ESP
    // flow report cannot appear to acknowledge bytes the sender has not recorded.
    job.bytesSent += chunk.length;
    job.statistics.add(chunk);
    recordOutstanding(job);
    await sendFrame(current.ws, chunk, { binary: true });
    recordOutstanding(job);
    if (job.firstPcmSentAt === null) {
      job.firstPcmSentAt = performance.now();
      logger.info({ playback_id: job.id, begin_to_first_pcm_ms: Math.round(job.firstPcmSentAt - job.beginSentAt) }, "Speaker first PCM sent");
    }
  }
  async function sendWithFlow(job, current, chunk) {
    let offset = 0;
    while (offset < chunk.length) {
      if (job.settled || connection !== current || current.ws.readyState !== 1) throw new Error("speaker disconnected");
      const nextLength = Math.min(chunkBytes, chunk.length - offset);
      const credit = speakerCreditBytes(
        job.bytesSent, job.receivedBytes, job.consumedBytes, job.reportedBufferedBytes,
        { receiverBufferTargetBytes, networkInFlightLimitBytes, maxOutstandingBytes },
      );
      if (credit < nextLength) { await waitForFlow(job); continue; }
      await sendPcmChunk(job, current, chunk.subarray(offset, offset + nextLength));
      offset += nextLength;
    }
  }
  function beginJob(job) {
    jobs.set(job.id, job);
    job.resultTimer = setTimeout(() => settle(job, { kind: "timeout", error: "speaker_stream_timeout" }), resultTimeoutMs);
    job.resultTimer.unref();
  }
  async function streamRealtime(job, current, source) {
    const iterator = source.audio[Symbol.asyncIterator]();
    let sourceCompleted = false;
    let cancellationPromise = null;
    const cancelSource = () => {
      source.cancel?.();
      if (typeof iterator.return !== "function") return Promise.resolve();
      cancellationPromise ??= iterator.return().catch((error) => {
        logger.warn({ playback_id: job.id, error_message: error?.message ?? "unknown" }, "Speaker source cancellation failed");
      });
      return cancellationPromise;
    };
    job.cancelSource = cancelSource;
    try {
      const first = await iterator.next();
      if (first.done || !Buffer.isBuffer(first.value) || !first.value.length) throw new Error("invalid_pcm");
      job.backendMetrics = source.metrics;
      beginJob(job);
      const begin = { type: "speaker_begin", playback_id: job.id, sample_rate: format.sampleRate, channels: 1, bits_per_sample: 16, streaming: true, max_bytes: maxStreamBytes };
      job.beginSentAt = performance.now();
      await sendFrame(current.ws, JSON.stringify(begin));
      logger.info({ device_id: current.deviceId, playback_id: job.id, streaming: true, max_pcm_bytes: maxStreamBytes }, "Speaker begin sent");
      await sendWithFlow(job, current, first.value);
      while (true) {
        const part = await iterator.next();
        if (part.done) { sourceCompleted = true; break; }
        await sendWithFlow(job, current, part.value);
      }
      if (!job.bytesSent || (job.bytesSent & 1)) throw new Error("invalid_pcm");
      job.ttsCompletedAt = source.metrics.completedAt ?? performance.now();
      job.audio = job.statistics.result(backend.limiter ?? SPEAKER_LIMITER);
      job.endSent = true;
      await sendFrame(current.ws, JSON.stringify({ type: "speaker_end", playback_id: job.id, bytes: job.bytesSent }));
      logStreamComplete(job, current);
      return job.completion;
    } finally {
      if (!sourceCompleted) await cancelSource();
      else if (cancellationPromise) await cancellationPromise;
      job.cancelSource = null;
    }
  }
  async function streamKnown(job, current, pcm) {
    job.pcmBytes = pcm.length;
    beginJob(job);
    job.beginSentAt = performance.now();
    await sendFrame(current.ws, JSON.stringify({ type: "speaker_begin", playback_id: job.id, sample_rate: format.sampleRate, channels: 1, bits_per_sample: 16, bytes: pcm.length }));
    logger.info({ device_id: current.deviceId, playback_id: job.id, pcm_bytes: pcm.length, streaming: false }, "Speaker begin sent");
    await sendWithFlow(job, current, pcm);
    job.audio = job.statistics.result(backend.limiter ?? SPEAKER_LIMITER);
    job.endSent = true;
    await sendFrame(current.ws, JSON.stringify({ type: "speaker_end", playback_id: job.id, bytes: job.bytesSent }));
    logStreamComplete(job, current);
    return job.completion;
  }
  function logStreamComplete(job, current) {
    const audio = job.audio;
    logger.info({
      event: "SPEAKER_AUDIO", playback_id: job.id,
      peak_dbfs: Number.isFinite(audio.peakDbfs) ? Number(audio.peakDbfs.toFixed(1)) : "-inf",
      rms_dbfs: Number.isFinite(audio.rmsDbfs) ? Number(audio.rmsDbfs.toFixed(1)) : "-inf",
      gain_db: backend.gainDb ?? SPEAKER_GAIN_DB, limiter: backend.limiter ?? SPEAKER_LIMITER,
      limiter_reached: audio.limiterReached, clipped: audio.clipped, bytes: audio.bytes,
    }, "SPEAKER_AUDIO");
    logger.info({
      device_id: current.deviceId, playback_id: job.id, bytes: job.bytesSent,
      stream_ms: Math.round(performance.now() - job.beginSentAt), pacing: "delivery_aware_receiver_credit",
      max_network_inflight_bytes: job.maxNetworkInFlightBytes,
      min_receiver_buffer_bytes: Number.isFinite(job.minReportedBufferedBytes) ? job.minReportedBufferedBytes : null,
      max_receiver_buffer_bytes: job.maxReportedBufferedBytes,
      receiver_buffer_target_bytes: receiverBufferTargetBytes,
      network_inflight_limit_bytes: networkInFlightLimitBytes,
      total_outstanding_high_water_bytes: job.totalOutstandingHighWaterBytes,
      total_outstanding_limit_bytes: maxOutstandingBytes,
      flow_reports: job.flowReports, received_flow_reports: job.receivedFlowReports,
      chunk_bytes: chunkBytes, producer_queue_high_water_bytes: job.backendMetrics?.producerQueueHighWaterBytes ?? null,
    }, "Speaker PCM stream sent");
  }
  async function run(job) {
    if (job.settled || closing) return;
    if (!getDevice()) throw new Error("speaker unavailable");
    logger.info({ playback_id: job.id, text_chars: job.text.length }, "Speaker TTS synthesis started");
    const connectionPromise = job.temporary ? requestTemporaryConnection(job) : waitForConnection();
    connectionPromise.catch(() => {});
    job.synthesisStartedAt = performance.now();
    if (typeof backend.stream === "function") {
      const [source, current] = await Promise.all([backend.stream(job.text, format), connectionPromise]);
      return streamRealtime(job, current, source);
    }
    const [pcm, current] = await Promise.all([backend.synthesize(job.text, format), connectionPromise]);
    job.ttsCompletedAt = performance.now();
    return streamKnown(job, current, pcm);
  }
  function speak(html, { maxChars = maxTextChars, temporary = false, replyReadyAt = performance.now(), metadata = null } = {}) {
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
      id, text, temporary, metadata, settled: false, resultTimer: null, queuedAt: performance.now(), replyReadyAt,
      synthesisStartedAt: null, ttsCompletedAt: null, beginSentAt: null, firstPcmSentAt: null,
      backendMetrics: null, playbackStartedAt: null, firstPcmToPlayMs: null, cancelSource: null,
      resolve, reject, completion, bytesSent: 0, receivedBytes: 0, consumedBytes: 0, reportedBufferedBytes: 0,
      flowVersion: 0, flowWaiters: new Set(), flowReports: 0, receivedFlowReports: 0, lastFlowReceivedBytes: 0,
      maxNetworkInFlightBytes: 0, totalOutstandingHighWaterBytes: 0, endSent: false,
      minReportedBufferedBytes: Number.POSITIVE_INFINITY, maxReportedBufferedBytes: 0, statistics: pcmStatistics(), audio: null,
    };
    allJobs.add(job);
    logger.info({ playback_id: id, text_chars: text.length, temporary }, "Speaker job queued");
    queue = queue.catch(() => {}).then(() => run(job)).catch((error) => {
      settle(job, { kind: "error", error: error?.code ?? error?.message ?? "TTS failed" });
      logger.warn({ playback_id: id, error_message: error?.code ?? error?.message ?? "unknown" }, "Speaker/TTS job failed");
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
      for (const job of jobs.values()) {
        void job.cancelSource?.();
        rejectFlowWaiters(job, new Error("speaker disconnected"));
      }
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
    job.firstPcmToPlayMs = message.first_pcm_to_play_ms;
    job.playbackStartedAt = performance.now();
    logger.info({
      event: "SPEAKER_TTFA", device_id: deviceId, playback_id: job.id,
      tts_request_to_first_pcm_ms: job.backendMetrics?.conditionerFirstOutputAt == null ? null : Math.round(job.backendMetrics.conditionerFirstOutputAt - job.backendMetrics.requestStartedAt),
      kokoro_first_audio_byte_ms: job.backendMetrics?.firstAudioByteAt == null ? null : Math.round(job.backendMetrics.firstAudioByteAt - job.backendMetrics.requestStartedAt),
      conditioning_first_output_ms: job.backendMetrics?.conditionerFirstOutputAt == null ? null : Math.round(job.backendMetrics.conditionerFirstOutputAt - job.backendMetrics.firstAudioByteAt),
      reply_ready_to_first_pcm_send_ms: Math.round(job.firstPcmSentAt - job.replyReadyAt),
      first_pcm_to_play_ms: job.firstPcmToPlayMs,
      estimated_reply_ready_to_audible_ms: Math.round(job.firstPcmSentAt - job.replyReadyAt + job.firstPcmToPlayMs),
      assistant_turn: job.metadata?.assistant_turn ?? false,
      voice_stream_id: job.metadata?.voice_stream_id ?? null,
      final_to_audible_ms: job.metadata?.final_at == null ? null : Math.round(performance.now() - job.metadata.final_at),
    }, "SPEAKER_TTFA");
    return true;
  }
  function handleResult(deviceId, message) {
    const job = jobs.get(message.playback_id);
    if (!job) return false;
    let result;
    if (message.type === "speaker_complete") {
      result = message.bytes === job.bytesSent ? { kind: "complete", bytes: message.bytes } : { kind: "error", error: "truncated" };
    } else result = { kind: "error", error: message.error ?? "device playback failed" };
    logger[result.kind === "complete" ? "info" : "warn"]({
      event: "SPEAKER_FLOW_FINAL", device_id: deviceId, playback_id: job.id,
      max_network_inflight_bytes: job.maxNetworkInFlightBytes,
      min_receiver_buffer_bytes: Number.isFinite(job.minReportedBufferedBytes) ? job.minReportedBufferedBytes : null,
      max_receiver_buffer_bytes: job.maxReportedBufferedBytes,
      receiver_buffer_target_bytes: receiverBufferTargetBytes,
      network_inflight_limit_bytes: networkInFlightLimitBytes,
      total_outstanding_high_water_bytes: job.totalOutstandingHighWaterBytes,
      total_outstanding_limit_bytes: maxOutstandingBytes,
      flow_reports: job.flowReports, received_flow_reports: job.receivedFlowReports,
      bytes_sent: job.bytesSent, bytes_received: job.receivedBytes, bytes_consumed: job.consumedBytes,
      result: result.kind, error: result.error,
    }, "SPEAKER_FLOW_FINAL");
    if (result.kind === "complete") {
      logger.info({
        event: "SPEAKER_TTFA_FINAL", device_id: deviceId, playback_id: job.id,
        tts_request_to_first_pcm_ms: job.backendMetrics?.conditionerFirstOutputAt == null ? null : Math.round(job.backendMetrics.conditionerFirstOutputAt - job.backendMetrics.requestStartedAt),
        conditioning_first_output_ms: job.backendMetrics?.conditionerFirstOutputAt == null ? null : Math.round(job.backendMetrics.conditionerFirstOutputAt - job.backendMetrics.firstAudioByteAt),
        reply_ready_to_first_pcm_send_ms: job.firstPcmSentAt == null ? null : Math.round(job.firstPcmSentAt - job.replyReadyAt),
        first_pcm_to_play_ms: job.firstPcmToPlayMs,
        estimated_reply_ready_to_audible_ms: job.firstPcmSentAt == null || job.firstPcmToPlayMs == null ? null : Math.round(job.firstPcmSentAt - job.replyReadyAt + job.firstPcmToPlayMs),
        full_tts_generation_ms: job.backendMetrics?.completedAt == null ? (job.ttsCompletedAt == null ? null : Math.round(job.ttsCompletedAt - job.synthesisStartedAt)) : Math.round(job.backendMetrics.completedAt - job.backendMetrics.requestStartedAt),
        total_playback_ms: job.beginSentAt == null ? null : Math.round(performance.now() - job.beginSentAt), bytes: message.bytes,
      }, "SPEAKER_TTFA_FINAL");
    }
    settle(job, result);
    logger[message.type === "speaker_complete" && result.kind === "complete" ? "info" : "warn"]({ device_id: deviceId, playback_id: message.playback_id, bytes: message.bytes, error: result.error }, `Speaker playback ${result.kind === "complete" ? "complete" : "failed"}`);
    return true;
  }
  function setPersistentEnabled(persistentEnabled) {
    if (persistentEnabled) return;
    for (const job of [...allJobs]) if (!job.temporary) settle(job, { kind: "disabled", error: "speaker disabled" });
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
