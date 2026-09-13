import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import path from "node:path";
import { Worker } from "node:worker_threads";

function normalize(vector) {
  const norm = Math.sqrt(vector.reduce((sum, value) => sum + value * value, 0));
  if (!Number.isFinite(norm) || norm === 0) throw new Error("Invalid speaker embedding");
  return vector.map((value) => value / norm);
}

function cosine(left, right) {
  if (!left || left.length !== right?.length) return null;
  return left.reduce((sum, value, index) => sum + value * right[index], 0);
}

export class NullSpeakerVerifier {
  async prewarm() {}
  isEnrolling() { return false; }
  status() { return { enabled: false, enrolled: false, enrollment_samples: 0 }; }
  async createSession() { return { enrollment: false, async acceptAudio() {}, async finish() { return { identity: "unknown", score: null, available: false }; }, async cancel() {} }; }
  async close() {}
}

export class SpeakerVerifier {
  constructor({ modelPath, storageDirectory, threshold = 0.65, numThreads = 1, logger, workerFactory }) {
    this.modelPath = path.resolve(modelPath);
    this.storageDirectory = path.resolve(storageDirectory);
    this.threshold = threshold;
    this.numThreads = numThreads;
    this.logger = logger;
    this.workerFactory = workerFactory ?? (() => new Worker(new URL("./speaker-verification-worker.js", import.meta.url), { workerData: { modelPath: this.modelPath, numThreads } }));
    this.worker = null;
    this.readyPromise = null;
    this.pending = new Map();
    this.nextRequestId = 1;
    this.enrollments = new Map();
    this.profiles = new Map();
  }

  profilePath(deviceId) { return path.join(this.storageDirectory, `${encodeURIComponent(deviceId)}-owner.json`); }

  async loadProfile(deviceId) {
    if (this.profiles.has(deviceId)) return this.profiles.get(deviceId);
    try {
      const parsed = JSON.parse(await readFile(this.profilePath(deviceId), "utf8"));
      const profile = { ...parsed, embedding: normalize(parsed.embedding.map(Number)) };
      this.profiles.set(deviceId, profile);
      return profile;
    } catch (error) {
      if (error?.code !== "ENOENT") this.logger?.warn?.({ device_id: deviceId, error_message: error.message }, "Ignoring invalid owner voiceprint");
      this.profiles.set(deviceId, null);
      return null;
    }
  }

  async prewarm() {
    if (this.readyPromise) return this.readyPromise;
    this.worker = this.workerFactory();
    this.readyPromise = new Promise((resolve, reject) => {
      this.worker.once("message", (message) => message.type === "ready" ? resolve() : reject(new Error("Speaker verifier failed to initialize")));
      this.worker.once("error", reject);
    });
    this.worker.on("message", (message) => {
      if (message.type === "ready") return;
      const pending = this.pending.get(message.requestId);
      if (!pending) return;
      this.pending.delete(message.requestId);
      message.error ? pending.reject(new Error(message.error)) : pending.resolve(message);
    });
    this.worker.on("error", (error) => {
      for (const pending of this.pending.values()) pending.reject(error);
      this.pending.clear();
      this.logger?.error?.({ error_message: error.message }, "Speaker verification worker failed");
    });
    return this.readyPromise;
  }

  async request(type, fields = {}, transfer = []) {
    await this.prewarm();
    const requestId = this.nextRequestId++;
    return new Promise((resolve, reject) => {
      this.pending.set(requestId, { resolve, reject });
      this.worker.postMessage({ type, requestId, ...fields }, transfer);
    });
  }

  beginEnrollment(deviceId) {
    this.enrollments.set(deviceId, { samples: [], required: 3, startedAt: new Date().toISOString() });
    return this.status(deviceId);
  }
  cancelEnrollment(deviceId) { return this.enrollments.delete(deviceId); }
  isEnrolling(deviceId) { return this.enrollments.has(deviceId); }
  status(deviceId) {
    const enrollment = this.enrollments.get(deviceId);
    const profile = this.profiles.get(deviceId);
    return { enabled: true, enrolled: Boolean(profile), threshold: this.threshold, enrollment_active: Boolean(enrollment), enrollment_samples: enrollment?.samples.length ?? 0, enrollment_required: enrollment?.required ?? 3 };
  }

  async storeEnrollmentSample(deviceId, embedding) {
    const enrollment = this.enrollments.get(deviceId);
    if (!enrollment) return null;
    enrollment.samples.push(normalize(Array.from(embedding)));
    if (enrollment.samples.length < enrollment.required) return this.status(deviceId);
    const averaged = normalize(enrollment.samples[0].map((_, index) => enrollment.samples.reduce((sum, sample) => sum + sample[index], 0) / enrollment.samples.length));
    const profile = { version: 1, owner: "owner", model: path.basename(this.modelPath), samples: enrollment.samples.length, created_at: new Date().toISOString(), embedding: averaged };
    await mkdir(this.storageDirectory, { recursive: true, mode: 0o700 });
    const target = this.profilePath(deviceId);
    const temporary = `${target}.${process.pid}.tmp`;
    await writeFile(temporary, `${JSON.stringify(profile)}\n`, { mode: 0o600 });
    await rename(temporary, target);
    this.profiles.set(deviceId, profile);
    this.enrollments.delete(deviceId);
    this.logger?.info?.({ event: "OWNER_ENROLLMENT_COMPLETE", device_id: deviceId, samples: profile.samples }, "OWNER_ENROLLMENT_COMPLETE");
    return this.status(deviceId);
  }

  async createSession({ deviceId, streamId, format }) {
    if (format.sampleRate !== 16_000 || format.channels !== 1 || format.bitsPerSample !== 16) throw new Error("Speaker verification requires mono 16 kHz PCM16");
    const enrollment = this.isEnrolling(deviceId);
    let transcript = "";
    await this.request("create", { sessionId: streamId });
    let finished = false;
    return {
      enrollment,
      setTranscript: (text) => { transcript = String(text ?? "").trim(); },
      acceptAudio: async (chunk) => {
        const bytes = Uint8Array.from(chunk);
        await this.request("audio", { sessionId: streamId, chunk: bytes.buffer }, [bytes.buffer]);
      },
      finish: async () => {
        if (finished) return null;
        finished = true;
        const result = await this.request("finish", { sessionId: streamId });
        if (!result.ready || !result.embedding) return { identity: "unknown", score: null, available: false, enrollment };
        if (enrollment) {
          if (!/^hi[,. ]+wall[ -]?e[.!?]*$/i.test(transcript)) {
            return { identity: "unknown", score: null, available: true, enrollment: true, rejected: "phrase_mismatch", transcript };
          }
          const status = await this.storeEnrollmentSample(deviceId, result.embedding);
          return { identity: "owner", score: 1, available: true, enrollment: true, enrollmentStatus: status };
        }
        const profile = await this.loadProfile(deviceId);
        const score = cosine(normalize(Array.from(result.embedding)), profile?.embedding);
        return { identity: score !== null && score >= this.threshold ? "owner" : "unknown", score, threshold: this.threshold, available: Boolean(profile), enrollment: false };
      },
      cancel: async () => { if (!finished) { finished = true; await this.request("cancel", { sessionId: streamId }); } },
    };
  }

  async close() { if (this.worker) await this.worker.terminate(); this.worker = null; }
}
