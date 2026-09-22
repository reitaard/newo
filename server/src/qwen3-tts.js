function ttsError(code, detail = "") {
  const error = new Error(detail || code);
  error.code = code;
  return error;
}

export class Qwen3TtsBackend {
  constructor({ baseUrl, path = "/v1/audio/speech", model = "qwen3-tts-0.6b-customvoice", speaker = "Serena",
    language = "Auto", speed = 1, responseFormat = "pcm", requestTimeoutMs = 30_000,
    maxPcmBytes = 2_880_000, fetchImpl = fetch, logger = null }) {
    this.name = "qwen3";
    this.baseUrl = String(baseUrl).replace(/\/+$/, "");
    this.path = path.startsWith("/") ? path : `/${path}`;
    this.model = model;
    this.voice = speaker;
    this.language = language;
    this.speed = speed;
    this.responseFormat = responseFormat;
    this.requestTimeoutMs = requestTimeoutMs;
    this.maxPcmBytes = maxPcmBytes;
    this.fetchImpl = fetchImpl;
    this.logger = logger;
    this.gainDb = 0;
    this.limiter = 1;
  }

  async stream(text, format, { signal, language = this.language, speed = this.speed } = {}) {
    if (format.sampleRate !== 24_000 || format.channels !== 1 || format.bitsPerSample !== 16) {
      throw ttsError("qwen3_tts_format_unsupported");
    }
    const controller = new AbortController();
    const relay = () => controller.abort(signal?.reason ?? ttsError("qwen3_tts_cancelled"));
    if (signal?.aborted) relay(); else signal?.addEventListener("abort", relay, { once: true });
    const timer = setTimeout(() => controller.abort(ttsError("qwen3_tts_timeout")), this.requestTimeoutMs);
    timer.unref?.();
    const metrics = {
      requestStartedAt: performance.now(),
      firstAudioByteAt: null,
      completedAt: null,
      rawPcmBytes: 0,
      conditionedPcmBytes: 0,
    };
    let response;
    try {
      response = await this.fetchImpl(`${this.baseUrl}${this.path}`, {
        method: "POST",
        signal: controller.signal,
        headers: { "content-type": "application/json", accept: "application/octet-stream" },
        body: JSON.stringify({ model: this.model, input: text, voice: this.voice, language, speed,
          response_format: this.responseFormat }),
      });
      if (!response.ok || !response.body) {
        throw ttsError("qwen3_tts_unavailable", `${response.status} ${await response.text()}`);
      }
    } catch (error) {
      clearTimeout(timer);
      signal?.removeEventListener("abort", relay);
      throw error;
    }

    const reader = response.body.getReader();
    const backend = this;
    return {
      metrics,
      cancel() { controller.abort(ttsError("qwen3_tts_cancelled")); },
      audio: (async function* () {
        try {
          while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            const chunk = Buffer.from(value);
            if (!chunk.length) continue;
            metrics.firstAudioByteAt ??= performance.now();
            metrics.rawPcmBytes += chunk.length;
            metrics.conditionedPcmBytes += chunk.length;
            if (metrics.rawPcmBytes > backend.maxPcmBytes) throw ttsError("qwen3_tts_too_large");
            yield chunk;
          }
          metrics.completedAt = performance.now();
        } finally {
          clearTimeout(timer);
          signal?.removeEventListener("abort", relay);
          await reader.cancel().catch(() => {});
        }
      })(),
    };
  }

  async streamSegments(segments, format, options = {}) {
    const backend = this;
    const metrics = { requestStartedAt: performance.now(), firstAudioByteAt: null, completedAt: null,
      rawPcmBytes: 0, conditionedPcmBytes: 0 };
    let current = null;
    let cancelled = false;
    return {
      metrics,
      cancel() { cancelled = true; current?.cancel?.(); },
      audio: (async function* () {
        for (const segment of segments) {
          if (cancelled) throw ttsError("qwen3_tts_cancelled");
          current = await backend.stream(segment.text, format, {
            ...options,
            language: segment.language ?? backend.language,
            speed: segment.speed ?? backend.speed,
          });
          for await (const chunk of current.audio) {
            metrics.firstAudioByteAt ??= current.metrics.firstAudioByteAt;
            metrics.rawPcmBytes += chunk.length;
            metrics.conditionedPcmBytes += chunk.length;
            yield chunk;
          }
        }
        metrics.completedAt = performance.now();
      })(),
    };
  }
}

export class FallbackTtsBackend {
  constructor(primary, fallback, logger = null) {
    this.name = `${primary.name}_fallback_${fallback.name}`;
    this.primary = primary;
    this.fallback = fallback;
    this.logger = logger;
    this.voice = primary.voice;
    this.gainDb = primary.gainDb;
    this.limiter = primary.limiter;
  }

  async stream(text, format, options) {
    try { return await this.primary.stream(text, format, options); }
    catch (error) {
      if (options?.signal?.aborted) throw error;
      this.logger?.warn?.({ event: "XIAOMEI_TTS_FALLBACK", error: error?.code ?? error?.message },
        "Serena unavailable; using fallback TTS");
      if (typeof this.fallback.stream === "function") return this.fallback.stream(text, format, options);
      const pcm = await this.fallback.synthesize(text, format);
      const now = performance.now();
      return { metrics: { requestStartedAt: now, firstAudioByteAt: now, completedAt: now,
        rawPcmBytes: pcm.length, conditionedPcmBytes: pcm.length },
        audio: (async function* () { yield pcm; })(), cancel() {} };
    }
  }
}
