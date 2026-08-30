import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import test from "node:test";
import { createDecoder } from "libopus-wasm";
import { createSpeakerRuntime } from "../src/tts.js";

// Asynchronous ESP model: admission is separate from 48 kB/s playback drain.
class FakeEsp extends EventEmitter {
  constructor(opus, { flowMs = 5 } = {}) { super(); this.readyState = 1; this.opus = opus; this.flowMs = flowMs; this.sent = []; this.onResult = null; this.chain = Promise.resolve(); }
  send(data, options, done) {
    const text = Buffer.isBuffer(data) ? null : String(data); this.sent.push({ data: Buffer.isBuffer(data) ? Buffer.from(data) : text, options });
    if (text?.includes("speaker_begin")) this.chain = this.chain.then(() => this.begin(JSON.parse(text)));
    else if (Buffer.isBuffer(data)) this.chain = this.chain.then(() => this.packet(data));
    else if (text?.includes("speaker_end")) this.chain = this.chain.then(() => { this.end = true; this.expected = JSON.parse(text).bytes; this.drain(); });
    done?.();
  }
  async begin(message) { this.id = message.playback_id; this.codec = message.codec ?? "pcm"; this.expected = 0; this.admitted = this.received = this.consumed = this.buffered = this.wire = this.packets = 0; this.sequence = 0; this.end = this.complete = false; if (this.codec === "opus") this.decoder = await createDecoder({ sampleRate: 24000, channels: 1, maxFrameSize: 960 }); this.flow(); }
  async packet(packet) {
    if (this.complete) throw new Error("late packet"); this.wire += packet.length;
    let valid = packet.length;
    if (this.codec === "opus") { assert.equal(packet.subarray(0, 4).toString(), "NWOP"); assert.equal(packet.readUInt16LE(4), this.sequence++); valid = packet.readUInt16LE(6); assert.ok(valid > 0 && valid <= 1920); const decoded = this.decoder.decode(packet.subarray(8)); assert.equal(decoded.length, 960); }
    this.admitted += valid; this.received += valid; this.buffered += valid; this.packets += 1;
  }
  flow() { if (this.complete || this.readyState !== 1) return; this.emit("message", Buffer.from(JSON.stringify({ type: "speaker_flow", playback_id: this.id, received_bytes: this.received, consumed_bytes: this.consumed, buffered_bytes: this.buffered, capacity_bytes: 24576 })), false); this.flowTimer = setTimeout(() => this.flow(), this.flowMs); }
  drain() { if (this.complete || this.readyState !== 1) return; const bytes = Math.min(this.buffered, 480); this.buffered -= bytes; this.consumed += bytes; if (this.end && this.buffered === 0) { assert.equal(this.received, this.expected); assert.equal(this.consumed, this.expected); this.complete = true; clearTimeout(this.flowTimer); this.decoder?.free(); this.onResult?.({ type: "speaker_complete", playback_id: this.id, bytes: this.consumed }); return; } this.drainTimer = setTimeout(() => this.drain(), 10); }
  close() { this.readyState = 3; clearTimeout(this.flowTimer); clearTimeout(this.drainTimer); this.decoder?.free(); this.emit("close"); }
}
const logger = new Proxy({}, { get: () => () => {} });
const backend = { limiter: .95, gainDb: 2, async synthesize() { return Buffer.alloc(2142, 4); } };
for (const opus of [true, false]) test(`native runtime ${opus ? "Opus" : "PCM fallback"} drains exactly without bootstrap`, async () => {
  const saved = process.env.SPEAKER_CODEC; process.env.SPEAKER_CODEC = "opus";
  const ws = new FakeEsp(opus); const runtime = createSpeakerRuntime({ logger, backend, enabled: true, getDevice: () => ({ ws }), sendControl: async () => true, resultTimeoutMs: 1500, flowTimeoutMs: 500 }); ws.onResult = (m) => runtime.handleResult("device", m);
  runtime.handleConnection(ws, "device"); ws.emit("message", Buffer.from(JSON.stringify({ type: "speaker_ready", codecs: opus ? ["pcm", "opus"] : ["pcm"] })), false);
  await runtime.speak("Native transport test.").completion; await runtime.speak("Native transport second playback.").completion;
  const begins = ws.sent.filter(({ data }) => typeof data === "string" && data.includes("speaker_begin")); assert.equal(begins.length, 2);
  const begin = JSON.parse(begins[0].data); assert.equal(begin.codec ?? "pcm", opus ? "opus" : "pcm");
  if (opus) { assert.equal(begin.opus_frame_ms, 40); assert.equal(begin.opus_bitrate, 24000); }
  assert.equal(ws.complete, true); assert.equal(ws.received, ws.consumed); runtime.close(); process.env.SPEAKER_CODEC = saved;
});
