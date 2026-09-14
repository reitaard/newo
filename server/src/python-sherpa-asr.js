import { spawn } from "node:child_process";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

const FORMAT = Object.freeze({ sampleRate: 16_000, channels: 1, bitsPerSample: 16 });

export class PythonSherpaAsrBackend {
  constructor(options, { logger, spawnProcess = spawn, startupTimeoutMs = 60_000 } = {}) {
    this.options = options;
    this.logger = logger;
    this.spawnProcess = spawnProcess;
    this.startupTimeoutMs = startupTimeoutMs;
    this.child = null;
    this.startPromise = null;
    this.requests = new Map();
    this.streams = new Map();
    this.nextRequestId = 1;
    this.closing = false;
    this.unavailableError = null;
  }

  async prewarm() {
    if (this.unavailableError) throw this.unavailableError;
    if (this.child && !this.startPromise) return;
    if (this.startPromise) return this.startPromise;
    const executable = this.options.pythonExecutable || "python3";
    const script = this.options.workerScript || fileURLToPath(new URL("../python/sherpa_streaming_worker.py", import.meta.url));
    const child = this.child = this.spawnProcess(executable, [script], {
      stdio: ["pipe", "pipe", "pipe"], windowsHide: true,
    });
    const startedAt = Date.now();
    this.logger?.info?.({ event: "SHERPA_PYTHON_STARTING", executable }, "SHERPA_PYTHON_STARTING");
    this.startPromise = new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`Python Sherpa startup timed out after ${this.startupTimeoutMs} ms`)), this.startupTimeoutMs);
      this.resolveStart = () => { clearTimeout(timer); resolve(); };
      this.rejectStart = (error) => { clearTimeout(timer); reject(error); };
    });
    createInterface({ input: child.stdout }).on("line", (line) => this.handleLine(child, line, startedAt));
    createInterface({ input: child.stderr }).on("line", (line) => {
      this.logger?.warn?.({ event: "SHERPA_PYTHON_STDERR", detail: line.slice(0, 1_000) }, "SHERPA_PYTHON_STDERR");
    });
    child.once("error", (error) => this.fail(child, error));
    child.once("exit", (code, signal) => {
      if (!this.closing && this.child === child) this.fail(child, new Error(`Python Sherpa worker exited (${code ?? signal})`));
    });
    child.stdin.write(`${JSON.stringify({ type: "init", options: this.options })}\n`);
    try { return await this.startPromise; }
    catch (error) { await this.close(); throw error; }
  }

  handleLine(child, line, startedAt) {
    if (this.child !== child) return;
    let message;
    try { message = JSON.parse(line); }
    catch { this.logger?.warn?.({ event: "SHERPA_PYTHON_OUTPUT", detail: line.slice(0, 1_000) }, "SHERPA_PYTHON_OUTPUT"); return; }
    if (message.type === "ready") {
      this.startPromise = null;
      this.resolveStart?.(); this.resolveStart = null; this.rejectStart = null;
      this.logger?.info?.({
        event: "SHERPA_READY", asr_backend: "sherpa-python", startup_ms: Date.now() - startedAt,
        asr_max_active_paths: message.max_active_paths,
        asr_lm_requested: message.lm_requested, asr_lm_enabled: message.lm_enabled,
        asr_lm_type: message.lm_type, asr_lm_path: message.lm_path,
        asr_lm_scale: message.lm_scale, asr_lm_reason: message.lm_reason,
        asr_worker_rss_bytes: message.rss_bytes, asr_worker_cpu_seconds: message.cpu_seconds,
      }, "SHERPA_READY");
      return;
    }
    if (message.type === "fatal") { this.fail(child, new Error(message.error || "Python Sherpa startup failed")); return; }
    if (message.type === "event") { this.streams.get(message.session_id)?.onEvent(message.event); return; }
    const pending = this.requests.get(message.request_id);
    if (!pending) return;
    this.requests.delete(message.request_id);
    this.logDecodeTelemetry(message);
    message.error ? pending.reject(new Error(message.error)) : pending.resolve(message);
  }

  logDecodeTelemetry(message) {
    if (message.decode_ms == null) return;
    this.logger?.info?.({ event: "SHERPA_PYTHON_DECODE", asr_backend: "sherpa-python",
      asr_decode_ms: message.decode_ms, asr_worker_rss_bytes: message.rss_bytes,
      asr_worker_cpu_seconds: message.cpu_seconds }, "SHERPA_PYTHON_DECODE");
  }

  fail(child, error) {
    if (this.child !== child || this.closing) return;
    this.unavailableError = error;
    this.rejectStart?.(error); this.startPromise = null; this.resolveStart = null; this.rejectStart = null;
    for (const pending of this.requests.values()) pending.reject(error);
    this.requests.clear();
    this.logger?.error?.({ event: "SHERPA_PYTHON_FAILED", error_message: error.message }, "SHERPA_PYTHON_FAILED");
  }

  async request(type, payload = {}) {
    await this.prewarm();
    if (!this.child?.stdin?.writable) throw new Error("Python Sherpa worker unavailable");
    const request_id = this.nextRequestId++;
    return new Promise((resolve, reject) => {
      this.requests.set(request_id, { resolve, reject });
      this.child.stdin.write(`${JSON.stringify({ type, request_id, ...payload })}\n`, (error) => {
        if (!error) return;
        this.requests.delete(request_id); reject(error);
      });
    });
  }

  async createStream({ format, onEvent }) {
    if (format.sampleRate !== FORMAT.sampleRate || format.channels !== FORMAT.channels || format.bitsPerSample !== FORMAT.bitsPerSample)
      throw new Error("Sherpa ASR requires mono 16 kHz signed 16-bit PCM");
    const response = await this.request("create", { format });
    const sessionId = response.session_id;
    this.streams.set(sessionId, { onEvent });
    return {
      acceptAudio: async (chunk) => { await this.request("audio", { session_id: sessionId, pcm: Buffer.from(chunk).toString("base64") }); },
      stop: async () => { try { await this.request("stop", { session_id: sessionId }); } finally { this.streams.delete(sessionId); } },
    };
  }

  getStatus() { return { configured: "sherpa-python", effective: "sherpa-python", online: Boolean(this.child && !this.unavailableError), fallback_active: false }; }

  async close() {
    this.closing = true;
    const child = this.child;
    this.child = null;
    this.startPromise = null;
    const error = new Error("Python Sherpa worker shutting down");
    this.rejectStart?.(error);
    for (const pending of this.requests.values()) pending.reject(error);
    this.requests.clear(); this.streams.clear();
    if (!child) return;
    try { child.stdin.write(`${JSON.stringify({ type: "shutdown" })}\n`); } catch {}
    const exited = new Promise((resolve) => {
      child.once("exit", resolve);
      setTimeout(resolve, 2_000).unref?.();
    });
    child.kill();
    await exited;
  }
}

export class LazyFallbackAsrBackend {
  constructor(primary, fallbackFactory, { logger } = {}) {
    this.active = primary;
    this.fallbackFactory = fallbackFactory;
    this.logger = logger;
    this.fallbackActive = false;
    this.startPromise = null;
  }
  async prewarm() {
    if (this.startPromise) return this.startPromise;
    this.startPromise = (async () => {
      try { await this.active.prewarm(); }
      catch (error) { await this.activateFallback(error); }
    })();
    try { await this.startPromise; } finally { this.startPromise = null; }
  }
  async activateFallback(error) {
    if (this.fallbackActive) throw error;
    this.logger?.error?.({ event: "VOICE_ASR_BACKEND_FALLBACK", from: "sherpa-python", to: "sherpa", error_message: error.message }, "VOICE_ASR_BACKEND_FALLBACK");
    await this.active.close?.();
    this.active = this.fallbackFactory();
    this.fallbackActive = true;
    await this.active.prewarm();
  }
  async createStream(options) {
    await this.prewarm();
    try { return await this.active.createStream(options); }
    catch (error) { await this.activateFallback(error); return this.active.createStream(options); }
  }
  getStatus() {
    const active = this.active.getStatus?.() ?? {};
    return { ...active, configured: "sherpa-python", effective: this.fallbackActive ? "sherpa" : "sherpa-python", fallback_active: this.fallbackActive };
  }
  async close() { await this.active.close?.(); }
}
