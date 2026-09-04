import { existsSync } from "node:fs";
import { mkdir, open } from "node:fs/promises";
import path from "node:path";
import { randomUUID } from "node:crypto";
import { createRequire } from "node:module";
import { Worker } from "node:worker_threads";

const require = createRequire(import.meta.url);

function wavHeader({ dataBytes, sampleRate, channels, bitsPerSample }) {
  const header = Buffer.alloc(44);
  const byteRate = sampleRate * channels * (bitsPerSample / 8);
  const blockAlign = channels * (bitsPerSample / 8);
  header.write("RIFF", 0);
  header.writeUInt32LE(36 + dataBytes, 4);
  header.write("WAVE", 8);
  header.write("fmt ", 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(byteRate, 28);
  header.writeUInt16LE(blockAlign, 32);
  header.writeUInt16LE(bitsPerSample, 34);
  header.write("data", 36);
  header.writeUInt32LE(dataBytes, 40);
  return header;
}

/** A safe no-transcription fallback for transport and capture debugging. */
export class NullAsrBackend {
  async prewarm() {}
  async createStream() {
    return { async acceptAudio() {}, async stop() {} };
  }
  async close() {}
}

function pcm16leToFloat32(chunk) {
  if (chunk.length % 2 !== 0) throw new Error("PCM16 chunk must contain an even number of bytes");
  const samples = new Float32Array(chunk.length / 2);
  for (let index = 0; index < samples.length; index += 1) samples[index] = chunk.readInt16LE(index * 2) / 32768;
  return samples;
}

/** Streaming sherpa-onnx adapter. One recognizer is shared, while every voice connection owns its stream. */
export class SherpaAsrBackend {
  constructor({ modelDirectory, model = "20m", numThreads = 2, provider = "cpu", hotwordsFile, hotwordsScore = 1.5 }) {
    this.sherpa = require("sherpa-onnx-node");
    this.modelDirectory = path.resolve(modelDirectory);
    const libriGiga = model === "libri-giga";
    const recognizerConfig = {
      featConfig: { sampleRate: 16_000, featureDim: 80 },
      modelConfig: {
        transducer: {
          encoder: path.join(this.modelDirectory, "encoder-epoch-99-avg-1.int8.onnx"),
          // The larger bundle's supported INT8 recipe retains the FP32 decoder.
          decoder: path.join(this.modelDirectory, libriGiga ? "decoder-epoch-99-avg-1.onnx" : "decoder-epoch-99-avg-1.int8.onnx"),
          joiner: path.join(this.modelDirectory, "joiner-epoch-99-avg-1.int8.onnx"),
        },
        tokens: path.join(this.modelDirectory, "tokens.txt"),
        numThreads,
        provider,
        debug: 0,
        modelType: "zipformer",
        ...(libriGiga ? {
          modelingUnit: "bpe",
          bpeVocab: path.join(this.modelDirectory, "bpe.vocab"),
        } : {}),
      },
      enableEndpoint: true,
      rule1MinTrailingSilence: 2.4,
      rule2MinTrailingSilence: 1.2,
      rule3MinUtteranceLength: 20,
    };
    if (hotwordsFile) {
      if (!libriGiga) throw new Error("Hotwords are currently supported only with the libri-giga BPE model");
      if (!existsSync(hotwordsFile)) throw new Error(`Configured hotwords file does not exist: ${hotwordsFile}`);
      recognizerConfig.decodingMethod = "modified_beam_search";
      recognizerConfig.maxActivePaths = 4;
      recognizerConfig.hotwordsFile = hotwordsFile;
      recognizerConfig.hotwordsScore = hotwordsScore;
    }
    this.recognizer = new this.sherpa.OnlineRecognizer(recognizerConfig);
  }

  async createStream({ format, onEvent }) {
    if (format.sampleRate !== 16_000 || format.channels !== 1 || format.bitsPerSample !== 16) {
      throw new Error("Sherpa ASR requires mono 16 kHz signed 16-bit PCM");
    }

    const recognizer = this.recognizer;
    const stream = recognizer.createStream();
    let partial = "";
    let final = "";
    const emit = (type, text) => {
      const normalized = String(text ?? "").trim();
      if (!normalized) return;
      if (type === "partial" && normalized === partial) return;
      if (type === "final" && normalized === final) return;
      if (type === "partial") partial = normalized;
      else final = normalized;
      // Keep the existing partial/final type for ESP compatibility. stage makes
      // the first pass explicit and reserves rescored_final for a future pass.
      onEvent({ type, stage: type === "final" ? "first_pass_final" : "partial", text: normalized });
    };
    const decode = () => {
      while (recognizer.isReady(stream)) recognizer.decode(stream);
      const text = recognizer.getResult(stream).text;
      emit("partial", text);
      if (recognizer.isEndpoint(stream)) {
        emit("final", text);
        recognizer.reset(stream);
        partial = "";
        final = "";
      }
    };

    return {
      async acceptAudio(chunk) {
        stream.acceptWaveform({ samples: pcm16leToFloat32(chunk), sampleRate: format.sampleRate });
        decode();
      },
      async stop() {
        stream.acceptWaveform({ samples: new Float32Array(6_400), sampleRate: format.sampleRate });
        decode();
        stream.inputFinished();
        decode();
        emit("final", recognizer.getResult(stream).text);
      },
    };
  }
}

// The native recognizer must never be constructed in the Fastify event-loop
// process. This proxy gives each real /voice connection a worker-owned stream.
export class WorkerAsrBackend {
  constructor(options, { logger, workerFactory } = {}) {
    this.options = options;
    this.logger = logger;
    this.workerFactory = workerFactory ?? (() => new Worker(new URL("./sherpa-worker.js", import.meta.url), { workerData: this.options }));
    this.worker = null;
    this.startPromise = null;
    this.resolveStart = null;
    this.rejectStart = null;
    this.startingAt = null;
    this.unavailableError = null;
    this.closing = false;
    this.requests = new Map();
    this.streams = new Map();
    this.nextId = 1;
  }

  async prewarm() {
    if (this.worker && !this.startPromise && !this.unavailableError) return;
    if (this.unavailableError) throw this.unavailableError;
    if (this.startPromise) return this.startPromise;

    this.startingAt = Date.now();
    this.logger?.info({ event: "SHERPA_WORKER_STARTING" }, "SHERPA_WORKER_STARTING");
    let worker;
    try { worker = this.worker = this.workerFactory(); }
    catch (error) {
      this.worker = null;
      this.unavailableError = error;
      this.logger?.error?.({ event: "SHERPA_START_FAILED", error_message: error.message }, "SHERPA_START_FAILED");
      throw error;
    }
    this.startPromise = new Promise((resolve, reject) => {
      this.resolveStart = resolve;
      this.rejectStart = reject;
    });
    worker.on("message", (message) => this.handleWorkerMessage(worker, message));
    worker.on("error", (error) => this.failWorker(worker, error));
    worker.on("exit", (code) => {
      if (this.worker !== worker) return;
      if (!this.closing && !this.unavailableError) this.failWorker(worker, new Error(`ASR worker exited (${code})`));
      this.worker = null;
    });
    return this.startPromise;
  }

  handleWorkerMessage(worker, message) {
    if (this.worker !== worker) return;
    if (message.type === "ready") {
      const startupMs = Math.max(0, Date.now() - this.startingAt);
      this.startPromise = null;
      this.resolveStart?.();
      this.resolveStart = null;
      this.rejectStart = null;
      this.logger?.info({ event: "SHERPA_READY", startup_ms: startupMs }, "SHERPA_READY");
      return;
    }
    if (message.type === "fatal") {
      this.failWorker(worker, new Error(message.error || "ASR worker fatal error"));
      return;
    }
    if (message.type === "event") { this.streams.get(message.sessionId)?.onEvent(message.event); return; }
    const pending = this.requests.get(message.requestId);
    if (!pending) return;
    this.requests.delete(message.requestId);
    message.error ? pending.reject(new Error(message.error)) : pending.resolve(message);
  }

  failWorker(worker, error) {
    if (this.worker !== worker || this.closing) return;
    this.unavailableError = error;
    this.rejectStart?.(error);
    this.startPromise = null;
    this.resolveStart = null;
    this.rejectStart = null;
    this.failAll(error);
    this.logger?.error?.({ event: "SHERPA_START_FAILED", error_message: error.message }, "SHERPA_START_FAILED");
  }

  failAll(error) {
    for (const pending of this.requests.values()) pending.reject(error);
    this.requests.clear();
  }

  async request(type, payload = {}, transfer = []) {
    await this.prewarm();
    if (!this.worker || this.unavailableError) throw this.unavailableError ?? new Error("ASR worker unavailable");
    const requestId = this.nextId++;
    return new Promise((resolve, reject) => {
      this.requests.set(requestId, { resolve, reject });
      try { this.worker.postMessage({ type, requestId, ...payload }, transfer); }
      catch (error) { this.requests.delete(requestId); reject(error); }
    });
  }

  async createStream({ format, onEvent }) {
    const response = await this.request("create", { format });
    const sessionId = response.sessionId;
    this.streams.set(sessionId, { onEvent });
    return {
      acceptAudio: async (chunk) => {
        // Copy exactly this bounded PCM chunk before transferring ownership.
        const bytes = Uint8Array.from(chunk);
        await this.request("audio", { sessionId, chunk: bytes.buffer }, [bytes.buffer]);
      },
      stop: async () => {
        try { await this.request("stop", { sessionId }); }
        finally { this.streams.delete(sessionId); }
      },
    };
  }

  async close() {
    this.closing = true;
    const error = new Error("ASR worker shutting down");
    this.rejectStart?.(error);
    this.failAll(error);
    this.streams.clear();
    const worker = this.worker;
    this.worker = null;
    this.startPromise = null;
    if (worker) await worker.terminate();
  }
}

class WavCapture {
  constructor(file, format) {
    this.file = file;
    this.format = format;
    this.handle = null;
    this.bytes = 0;
    this.writeChain = Promise.resolve();
  }

  async start() {
    await mkdir(path.dirname(this.file), { recursive: true, mode: 0o700 });
    this.handle = await open(this.file, "w", 0o600);
    await this.handle.write(wavHeader({ ...this.format, dataBytes: 0 }), 0);
  }

  write(chunk) {
    this.bytes += chunk.length;
    this.writeChain = this.writeChain.then(() => this.handle.write(chunk));
    return this.writeChain;
  }

  async close() {
    if (!this.handle) return;
    await this.writeChain;
    await this.handle.write(wavHeader({ ...this.format, dataBytes: this.bytes }), 0);
    await this.handle.close();
    this.handle = null;
  }
}

export function createVoiceRuntime({ logger, config, asr = new NullAsrBackend() }) {
  const format = Object.freeze({
    sampleRate: config.sampleRate,
    channels: config.channels,
    bitsPerSample: config.bitsPerSample,
  });
  const bytesPerSecond = format.sampleRate * format.channels * (format.bitsPerSample / 8);

  function logTranscript(deviceId, streamId, event, timings) {
    const text = String(event.text ?? "").trim();
    if (!text || !["partial", "final"].includes(event.type)) return;
    const fields = { device_id: deviceId, stream_id: streamId, transcript_type: event.type, transcript_stage: event.stage, transcript: text };
    if (config.liveTestMode) Object.assign(fields, timings);
    logger.info(fields, `${event.type.toUpperCase()}: ${text}`);
  }

  async function handleConnection(ws, deviceId) {
    const streamId = randomUUID();
    const connectedAt = Date.now();
    let startedAt = null;
    let bytesReceived = 0;
    let capture = null;
    let asrStream = null;
    let nextProgressBytes = bytesPerSecond * 5;
    let firstPartialMs = null;
    let finalMs = null;
    let finalTranscript = null;
    let assistantFinalDelivered = false;
    let processing = Promise.resolve();
    let queuedChunks = 0;
    let closed = false;
    let closeRequested = false;
    let cleanupPromise = null;
    let noAudioTimer = null;

    const requestClose = (code, reason) => {
      if (closeRequested) return;
      closeRequested = true;
      if (ws.readyState === 1) ws.close(code, reason);
    };
    const stop = (outcome) => {
      if (cleanupPromise) return cleanupPromise;
      cleanupPromise = (async () => {
        if (noAudioTimer) { clearTimeout(noAudioTimer); noAudioTimer = null; }
        // Do not race recognizer teardown with an in-flight acceptAudio().
        await processing.catch(() => {});
        if (closed) return;
        closed = true;
        const durationMs = Math.round((bytesReceived / bytesPerSecond) * 1_000);
        try {
          await asrStream?.stop();
        } catch (error) {
          logger.warn({ device_id: deviceId, stream_id: streamId, error_message: error?.message ?? "unknown" }, "Voice ASR stream cleanup failed");
        } finally {
          try {
            await capture?.close();
          } catch (error) {
            logger.warn({ device_id: deviceId, stream_id: streamId, error_message: error?.message ?? "unknown" }, "Voice WAV cleanup failed");
          }
        }
        if (asrStream) logger.info({ old_stream_id: streamId, device_id: deviceId, outcome }, "VOICE_ASR_STREAM_CLOSED");
        asrStream = null;
        const fields = { device_id: deviceId, stream_id: streamId, bytes_received: bytesReceived, audio_duration_ms: durationMs, outcome };
        if (config.liveTestMode) Object.assign(fields, { first_partial_ms: firstPartialMs, final_ms: finalMs, final_transcript: finalTranscript });
        logger.info(fields, "Voice stream stopped");
      })();
      return cleanupPromise;
    };

    logger.info({ event: "VOICE_CONNECTED", device_id: deviceId, stream_id: streamId, format }, "Voice device connected");

    ws.on("message", (raw, isBinary) => {
      // Do not allow WebSocket arrivals to form an unbounded Promise/PCM queue
      // while the worker is decoding. Realtime audio is failed, not buffered.
      if (isBinary && !closed && ++queuedChunks > (config.maxPendingChunks ?? 2)) {
        queuedChunks -= 1;
        logger.warn({ device_id: deviceId, stream_id: streamId }, "Voice worker backpressure limit reached");
        requestClose(1013, "voice worker busy");
        return;
      }
      processing = processing.then(async () => {
      if (!isBinary || closed) {
        if (!isBinary) logger.warn({ device_id: deviceId, stream_id: streamId }, "Ignoring non-binary voice message");
        return;
      }

      try {
        const chunk = Buffer.from(raw);
        if (chunk.length === 0) return;
        if (bytesReceived + chunk.length > config.maxStreamBytes) {
          logger.warn({ device_id: deviceId, stream_id: streamId, bytes_received: bytesReceived }, "Voice stream exceeded configured size limit");
          requestClose(1009, "voice stream too large");
          return;
        }

        if (!startedAt) {
          startedAt = Date.now();
          if (noAudioTimer) { clearTimeout(noAudioTimer); noAudioTimer = null; }
          logger.info({
            event: "VOICE_FIRST_AUDIO",
            device_id: deviceId,
            stream_id: streamId,
            chunk_bytes: chunk.length,
            connection_to_first_audio_ms: Math.max(0, startedAt - connectedAt),
          }, "VOICE_FIRST_AUDIO");
          asrStream = await asr.createStream({
            deviceId,
            streamId,
            format,
            onEvent(event) {
              const elapsedMs = Math.max(0, Date.now() - startedAt);
              const audioDurationMs = Math.round((bytesReceived / bytesPerSecond) * 1_000);
              if (event.type === "partial" && firstPartialMs === null) {
                firstPartialMs = elapsedMs;
                logger.info({ event: "VOICE_FIRST_PARTIAL", device_id: deviceId, stream_id: streamId, elapsed_ms: elapsedMs }, "VOICE_FIRST_PARTIAL");
              }
              if (event.type === "final" && finalMs === null) {
                finalMs = elapsedMs;
                finalTranscript = event.text;
                logger.info({ event: "VOICE_FINAL", device_id: deviceId, stream_id: streamId, elapsed_ms: elapsedMs }, "VOICE_FINAL");
              }
              logTranscript(deviceId, streamId, event, {
                elapsed_ms: elapsedMs,
                audio_duration_ms: audioDurationMs,
                first_partial_ms: firstPartialMs,
                final_ms: finalMs,
              });
              // `type` remains partial/final for deployed ESP clients. `stage`
              // allows a later rescorer to emit rescored_final without a wire break.
              if (ws.readyState === 1) ws.send(JSON.stringify(event));
              // Cleanup can generate another final event. Exactly one finalized
              // utterance may leave this stream for the assistant.
              if (event.type === "final" && !assistantFinalDelivered && typeof config.onFinalTranscript === "function") {
                assistantFinalDelivered = true;
                void Promise.resolve()
                  .then(() => config.onFinalTranscript({ deviceId, streamId, text: event.text, asrFinalMs: finalMs }))
                  .catch((error) => logger.warn({ device_id: deviceId, stream_id: streamId, error_message: error?.message ?? "unknown" }, "Voice final callback failed"));
              }
            },
          });
          if (config.saveWav) {
            const file = path.join(config.captureDirectory, `${new Date().toISOString().replaceAll(/[:.]/g, "-")}-${streamId}.wav`);
            capture = new WavCapture(file, format);
            await capture.start();
            logger.info({ device_id: deviceId, stream_id: streamId, capture_file: file }, "Voice WAV capture started");
          }
          logger.info({ event: "VOICE_ASR_STREAM_CREATED", new_stream_id: streamId, device_id: deviceId, format, connection_to_stream_ms: Math.max(0, Date.now() - connectedAt) }, "VOICE_ASR_STREAM_CREATED");
          logger.info({ device_id: deviceId, stream_id: streamId, format }, "Voice stream started");
        }

        bytesReceived += chunk.length;
        await capture?.write(chunk);
        await asrStream.acceptAudio(chunk);
        if (bytesReceived >= nextProgressBytes) {
          const audioDurationMs = Math.round((bytesReceived / bytesPerSecond) * 1_000);
          logger.info({ device_id: deviceId, stream_id: streamId, bytes_received: bytesReceived, audio_duration_ms: audioDurationMs }, "Voice audio received");
          nextProgressBytes = bytesReceived + (bytesPerSecond * 5);
        }
      } catch (error) {
        logger.warn({ device_id: deviceId, stream_id: streamId, error_message: error?.message ?? "unknown" }, "Voice stream processing failed");
        requestClose(1011, "voice stream processing failed");
      } finally {
        if (isBinary) queuedChunks = Math.max(0, queuedChunks - 1);
      }
      });
    });

    ws.on("close", (code) => { void stop(`disconnect:${code}`); });
    ws.on("error", (error) => {
      logger.warn({ device_id: deviceId, stream_id: streamId, error_message: error.message }, "Voice WebSocket error");
      requestClose(1011, "voice websocket error");
      void stop("error");
    });

    noAudioTimer = setTimeout(() => {
      noAudioTimer = null;
      if (startedAt || closed) return;
      logger.warn({ event: "VOICE_NO_AUDIO_TIMEOUT", device_id: deviceId, stream_id: streamId, timeout_ms: config.firstAudioTimeoutMs ?? 2_500 }, "voice_no_audio_timeout");
      requestClose(1008, "voice no audio");
    }, config.firstAudioTimeoutMs ?? 2_500);
    noAudioTimer.unref?.();
  }

  return { handleConnection };
}
