/** Local Pocket TTS float32 streaming adapter. Output is canonical 24 kHz mono PCM16LE. */
export const POCKET_SAMPLE_RATE = 24_000;

function abortError(code) {
  const error = new Error(code);
  error.code = code;
  return error;
}

export class Float32ToPcm16 {
  #carry = Buffer.alloc(0);

  push(value) {
    if (!Buffer.isBuffer(value) && !(value instanceof Uint8Array)) throw new Error("invalid Pocket float32 chunk");
    let input = Buffer.from(value);
    if (this.#carry.length) input = Buffer.concat([this.#carry, input]);
    const usable = input.length - (input.length % 4);
    this.#carry = input.subarray(usable);
    if (!usable) return Buffer.alloc(0);
    const output = Buffer.allocUnsafe((usable / 4) * 2);
    for (let source = 0, target = 0; source < usable; source += 4, target += 2) {
      const sample = input.readFloatLE(source);
      if (!Number.isFinite(sample)) throw new Error("invalid Pocket float32 sample");
      const clipped = Math.max(-1, Math.min(1, sample));
      output.writeInt16LE(Math.round(clipped < 0 ? clipped * 32_768 : clipped * 32_767), target);
    }
    return output;
  }

  finish() {
    if (this.#carry.length) throw new Error("incomplete Pocket float32 sample");
  }
}

/**
 * Consumes the localhost-only Python service incrementally. No ffmpeg, resampling,
 * WAV creation, or complete-utterance buffering occurs on this path.
 */
export class PocketTtsBackend {
  constructor({
    baseUrl = "http://127.0.0.1:8123", voice = "michael", requestTimeoutMs = 30_000,
    streamNoProgressMs = 10_000, streamAbsoluteMs = 70_000, maxPcmBytes = 2_880_000, logger = null,
  } = {}) {
    this.baseUrl = String(baseUrl).replace(/\/+$/, "");
    this.voice = voice;
    this.requestTimeoutMs = requestTimeoutMs;
    this.streamNoProgressMs = streamNoProgressMs;
    this.streamAbsoluteMs = streamAbsoluteMs;
    this.maxPcmBytes = maxPcmBytes;
    this.logger = logger;
    // Pocket Michael is intentionally raw: old Kokoro high-pass/gain/limiter are not applied.
    this.gainDb = 0;
    this.limiter = 1;
  }

  async stream(text, format) {
    if (format?.sampleRate !== POCKET_SAMPLE_RATE || format?.channels !== 1 || format?.bitsPerSample !== 16) {
      throw new Error("Pocket requires canonical mono 24000 Hz PCM16 speaker output");
    }
    const controller = new AbortController();
    const metrics = { requestStartedAt: performance.now(), responseHeadersAt: null, firstAudioByteAt: null,
      conditionerFirstOutputAt: null, completedAt: null, rawFloat32Bytes: 0, conditionedPcmBytes: 0,
      producerQueueHighWaterBytes: 0 };
    const requestTimer = setTimeout(() => controller.abort(abortError("pocket_request_timeout")), this.requestTimeoutMs);
    const absoluteTimer = setTimeout(() => controller.abort(abortError("pocket_stream_timeout")), this.streamAbsoluteMs);
    requestTimer.unref(); absoluteTimer.unref();
    this.logger?.info({ event: "POCKET_REQUEST", voice: this.voice, text_chars: String(text).length }, "Pocket synthesis requested");
    let response;
    try {
      response = await fetch(`${this.baseUrl}/v1/audio/newo-stream`, {
        method: "POST", headers: { "content-type": "application/json", accept: "application/octet-stream" },
        body: JSON.stringify({ input: text, voice: this.voice }), signal: controller.signal,
      });
    } catch (error) {
      clearTimeout(requestTimer); clearTimeout(absoluteTimer);
      if (controller.signal.aborted) throw abortError(controller.signal.reason?.code ?? "pocket_request_timeout");
      throw new Error(`Pocket unavailable: ${error?.message ?? "request failed"}`);
    }
    clearTimeout(requestTimer);
    metrics.responseHeadersAt = performance.now();
    if (!response.ok) {
      clearTimeout(absoluteTimer);
      const detail = await response.text().catch(() => "");
      throw new Error(`Pocket request failed (${response.status})${detail ? `: ${detail.slice(0, 500)}` : ""}`);
    }
    const contentType = response.headers.get("content-type")?.split(";", 1)[0].trim().toLowerCase();
    const rate = response.headers.get("x-audio-sample-rate");
    const channels = response.headers.get("x-audio-channels");
    const audioFormat = response.headers.get("x-audio-format");
    if (contentType !== "application/octet-stream" || rate !== "24000" || channels !== "1" || audioFormat !== "pcm_f32le") {
      clearTimeout(absoluteTimer); await response.body?.cancel?.().catch(() => {});
      throw new Error("Pocket returned unexpected raw audio format");
    }
    if (!response.body) { clearTimeout(absoluteTimer); throw new Error("Pocket returned no audio body"); }

    const backend = this;
    let cancelled = false;
    async function* audio() {
      const reader = response.body.getReader();
      const converter = new Float32ToPcm16();
      try {
        while (true) {
          let timer;
          const result = await Promise.race([
            reader.read(),
            new Promise((_, reject) => { timer = setTimeout(() => { controller.abort(abortError("pocket_stream_timeout")); reject(abortError("pocket_stream_timeout")); }, backend.streamNoProgressMs); timer.unref(); }),
          ]).finally(() => clearTimeout(timer));
          if (result.done) break;
          const chunk = Buffer.from(result.value);
          if (metrics.firstAudioByteAt === null) metrics.firstAudioByteAt = performance.now();
          metrics.rawFloat32Bytes += chunk.length;
          // Four float bytes become two PCM bytes; cap both representations safely.
          if (metrics.rawFloat32Bytes > backend.maxPcmBytes * 2) throw new Error("Pocket output exceeded limit");
          const pcm = converter.push(chunk);
          if (!pcm.length) continue;
          metrics.conditionedPcmBytes += pcm.length;
          if (metrics.conditionedPcmBytes > backend.maxPcmBytes) throw new Error("Pocket output exceeded limit");
          if (metrics.conditionerFirstOutputAt === null) {
            metrics.conditionerFirstOutputAt = performance.now();
            backend.logger?.info({ event: "POCKET_FIRST_PCM", voice: backend.voice,
              request_to_first_pcm_ms: Math.round(metrics.conditionerFirstOutputAt - metrics.requestStartedAt) }, "Pocket first PCM converted");
          }
          yield pcm;
        }
        converter.finish();
        if (!metrics.conditionedPcmBytes) throw new Error("Pocket returned empty audio");
        metrics.completedAt = performance.now();
        backend.logger?.info({ event: "POCKET_DONE", voice: backend.voice,
          request_to_first_pcm_ms: Math.round(metrics.conditionerFirstOutputAt - metrics.requestStartedAt),
          synthesis_ms: Math.round(metrics.completedAt - metrics.requestStartedAt),
          audio_ms: Math.round(metrics.conditionedPcmBytes / 48), pcm_bytes: metrics.conditionedPcmBytes }, "Pocket synthesis completed");
      } finally {
        clearTimeout(absoluteTimer);
        if (!controller.signal.aborted) controller.abort(abortError(cancelled ? "pocket_stream_cancelled" : "pocket_stream_closed"));
        await reader.cancel().catch(() => {});
        reader.releaseLock();
      }
    }
    return { audio: audio(), metrics, streaming: true, cancel() { cancelled = true; controller.abort(abortError("pocket_stream_cancelled")); } };
  }
}
