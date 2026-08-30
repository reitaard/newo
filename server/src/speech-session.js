import { splitRealtimeText } from "./tts.js";

// Bounded clause accumulator for a future token-streaming LLM source. It does
// not own a TTS request: callers enqueue each emitted clause and retain normal
// speaker cancellation/flow ownership.
export class SpeechSession {
  constructor({ onClause, maximumChars = 512, minimumChars = 14 } = {}) {
    if (typeof onClause !== "function" || !Number.isInteger(maximumChars) || maximumChars < minimumChars) throw new Error("invalid speech session");
    this.onClause = onClause; this.maximumChars = maximumChars; this.minimumChars = minimumChars;
    this.pending = ""; this.cancelled = false; this.finished = false;
  }
  pushText(chunk) {
    if (this.cancelled || this.finished) return [];
    this.pending = `${this.pending}${String(chunk ?? "")}`.replace(/\s+/g, " ").trimStart();
    if (this.pending.length > this.maximumChars) return this.flush(true);
    return this.flush(false);
  }
  finish() { if (this.cancelled || this.finished) return []; this.finished = true; return this.flush(true); }
  cancel() { this.cancelled = true; this.pending = ""; }
  flush(force) {
    const boundary = [...this.pending.matchAll(/[.!?]["']?(?=\s|$)/g)].at(-1);
    if (!force && (!boundary || boundary.index + boundary[0].length < this.minimumChars)) return [];
    const take = force ? this.pending.length : boundary.index + boundary[0].length;
    const text = this.pending.slice(0, take).trim(); this.pending = this.pending.slice(take).trimStart();
    if (!text) return [];
    // Retain the shared bounded segmentation policy even for a long clause.
    const clauses = splitRealtimeText(text).filter((clause) => clause.length >= this.minimumChars || force);
    for (const clause of clauses) this.onClause(clause);
    return clauses;
  }
}
