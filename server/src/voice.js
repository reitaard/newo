import { mkdir, open } from "node:fs/promises";
import path from "node:path";
import { randomUUID } from "node:crypto";
import { createRequire } from "node:module";

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
  async createStream() {
    return { async acceptAudio() {}, async stop() {} };
  }
}

function pcm16leToFloat32(chunk) {
  if (chunk.length % 2 !== 0) throw new Error("PCM16 chunk must contain an even number of bytes");
  const samples = new Float32Array(chunk.length / 2);
  for (let index = 0; index < samples.length; index += 1) samples[index] = chunk.readInt16LE(index * 2) / 32768;
  return samples;
}

/** Streaming sherpa-onnx adapter. One recognizer is shared, while every voice connection owns its stream. */
export class SherpaAsrBackend {
  constructor({ modelDirectory, numThreads = 2, provider = "cpu" }) {
    this.sherpa = require("sherpa-onnx-node");
    this.modelDirectory = path.resolve(modelDirectory);
    this.recognizer = new this.sherpa.OnlineRecognizer({
      featConfig: { sampleRate: 16_000, featureDim: 80 },
      modelConfig: {
        transducer: {
          encoder: path.join(this.modelDirectory, "encoder-epoch-99-avg-1.int8.onnx"),
          decoder: path.join(this.modelDirectory, "decoder-epoch-99-avg-1.int8.onnx"),
          joiner: path.join(this.modelDirectory, "joiner-epoch-99-avg-1.int8.onnx"),
        },
        tokens: path.join(this.modelDirectory, "tokens.txt"),
        numThreads,
        provider,
        debug: 0,
        modelType: "zipformer",
      },
      enableEndpoint: true,
      rule1MinTrailingSilence: 2.4,
      rule2MinTrailingSilence: 1.2,
      rule3MinUtteranceLength: 20,
    });
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
      onEvent({ type, text: normalized });
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

  function logTranscript(deviceId, event) {
    const text = String(event.text ?? "").trim();
    if (!text || !["partial", "final"].includes(event.type)) return;
    logger.info({ device_id: deviceId, transcript_type: event.type, transcript: text }, `${event.type.toUpperCase()}: ${text}`);
  }

  async function handleConnection(ws, deviceId) {
    const streamId = randomUUID();
    let startedAt = null;
    let bytesReceived = 0;
    let capture = null;
    let asrStream = null;
    let nextProgressBytes = bytesPerSecond * 5;
    let processing = Promise.resolve();
    let closed = false;

    const stop = async (outcome) => {
      if (closed) return;
      closed = true;
      const durationMs = Math.round((bytesReceived / bytesPerSecond) * 1_000);
      try {
        await asrStream?.stop();
        await capture?.close();
      } catch (error) {
        logger.warn({ device_id: deviceId, stream_id: streamId, error_message: error?.message ?? "unknown" }, "Voice stream cleanup failed");
      }
      logger.info({ device_id: deviceId, stream_id: streamId, bytes_received: bytesReceived, audio_duration_ms: durationMs, outcome }, "Voice stream stopped");
    };

    logger.info({ device_id: deviceId, stream_id: streamId, format }, "Voice device connected");

    ws.on("message", (raw, isBinary) => {
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
          ws.close(1009, "voice stream too large");
          return;
        }

        if (!startedAt) {
          startedAt = Date.now();
          asrStream = await asr.createStream({
            deviceId,
            streamId,
            format,
            onEvent(event) {
              logTranscript(deviceId, event);
              if (ws.readyState === 1) ws.send(JSON.stringify({ type: "transcript", ...event }));
            },
          });
          if (config.saveWav) {
            const file = path.join(config.captureDirectory, `${new Date().toISOString().replaceAll(/[:.]/g, "-")}-${streamId}.wav`);
            capture = new WavCapture(file, format);
            await capture.start();
            logger.info({ device_id: deviceId, stream_id: streamId, capture_file: file }, "Voice WAV capture started");
          }
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
        ws.close(1011, "voice stream processing failed");
      }
      });
    });

    ws.on("close", (code) => { void processing.then(() => stop(`disconnect:${code}`)); });
    ws.on("error", (error) => {
      logger.warn({ device_id: deviceId, stream_id: streamId, error_message: error.message }, "Voice WebSocket error");
      void stop("error");
    });
  }

  return { handleConnection };
}
