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
    else if (text?.includes("speaker_end")) this.chain = this.chain.then(() => { this.end = true; this.expected = JSON.parse(text).bytes; this.startDrain(); });
    done?.();
  }
  async begin(message) { this.id = message.playback_id; this.codec = message.codec ?? "pcm"; this.expected = 0; this.admitted = this.received = this.consumed = this.buffered = this.wire = this.packets = 0; this.sequence = 0; this.opusQueue = []; this.queueHigh = 0; this.end = this.complete = this.draining = this.started = false; if (this.codec === "opus") this.decoder = await createDecoder({ sampleRate: 24000, channels: 1, maxFrameSize: 960 }); this.flow(); }
  async packet(packet) {
    if (this.complete) throw new Error("late packet"); this.wire += packet.length;
    let valid = packet.length;
    if (this.codec === "opus") { assert.equal(packet.subarray(0, 4).toString(), "NWOP"); assert.equal(packet.readUInt16LE(4), this.sequence++); valid = packet.readUInt16LE(6); assert.ok(valid > 0 && valid <= 1920); const decoded = this.decoder.decode(packet.subarray(8)); assert.equal(decoded.length, 960); this.opusQueue.push(valid); this.queueHigh = Math.max(this.queueHigh, this.opusQueue.length); }
    this.admitted += valid; this.packets += 1;
    if (this.codec === "pcm") { this.received += valid; this.buffered += valid; }
    if (this.codec === "pcm" || this.opusQueue.length >= 25) this.startDrain();
  }
  flow() { if (this.complete || this.readyState !== 1) return; this.emit("message", Buffer.from(JSON.stringify({ type: "speaker_flow", playback_id: this.id, admitted_bytes: this.admitted, received_bytes: this.received, consumed_bytes: this.consumed, buffered_bytes: this.buffered, opus_queued_packets: this.opusQueue.length, opus_queued_pcm_bytes: this.opusQueue.reduce((sum, value) => sum + value, 0), capacity_bytes: 24576 })), false); this.flowTimer = setTimeout(() => this.flow(), this.flowMs); }
  startDrain() { if (!this.draining) { this.draining = true; this.drain(); } }
  drain() { if (this.complete || this.readyState !== 1) return; while (this.opusQueue.length && this.buffered + this.opusQueue[0] <= 24576) { const valid = this.opusQueue.shift(); this.received += valid; this.buffered += valid; } if (!this.started && this.buffered >= 12_288) { this.started = true; this.onStarted?.({ playback_id: this.id, first_pcm_to_play_ms: 1000 }); } const bytes = Math.min(this.buffered, 480); this.buffered -= bytes; this.consumed += bytes; if (this.end && this.opusQueue.length === 0 && this.buffered === 0) { assert.equal(this.received, this.expected); assert.equal(this.consumed, this.expected); this.complete = true; clearTimeout(this.flowTimer); this.decoder?.free(); this.onResult?.({ type: "speaker_complete", playback_id: this.id, bytes: this.consumed }); return; } this.drainTimer = setTimeout(() => this.drain(), 10); }
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
test("native Opus flow credit progresses while realtime drain is active", async () => {
  const saved = process.env.SPEAKER_CODEC; process.env.SPEAKER_CODEC = "opus";
  const events = [];
  const ws = new FakeEsp(true); const runtime = createSpeakerRuntime({ logger: { info(value) { events.push(value); }, warn(value) { events.push(value); } }, backend: { ...backend, async stream() { return { metrics: {}, audio: (async function* () { yield Buffer.alloc(38_400, 3); yield Buffer.alloc(38_400, 3); })() }; } }, enabled: true, getDevice: () => ({ ws }), sendControl: async () => true, resultTimeoutMs: 4000, flowTimeoutMs: 500 }); ws.onResult = (m) => runtime.handleResult("device", m); ws.onStarted = (m) => runtime.handlePlaybackStarted("device", m);
  try {
    runtime.handleConnection(ws, "device"); ws.emit("message", Buffer.from(JSON.stringify({ type: "speaker_ready", codecs: ["pcm", "opus"] })), false);
    await runtime.speak("Long flow-credit test.").completion;
    assert.ok(ws.queueHigh >= 25); assert.equal(ws.received, 76_800); assert.equal(ws.consumed, 76_800); assert.equal(ws.buffered, 0);
    const pacer = events.find((event) => event.event === "SPEAKER_PACER");
    assert.equal(pacer.initial_pcm_bytes, 48_000); assert.equal(pacer.catchup_frames, 0);
    const flow = events.find((event) => event.event === "SPEAKER_FLOW_FINAL");
    assert.ok(Number.isInteger(flow.opus_queue_min_active_packets));
    assert.ok(Number.isInteger(flow.opus_queue_min_active_pcm_bytes));
    assert.ok(Number.isInteger(flow.min_active_total_reservoir_bytes));
    assert.ok(flow.max_active_total_reservoir_bytes >= flow.min_active_total_reservoir_bytes);
    assert.ok(Number.isInteger(flow.low_reservoir_events_600ms));
    assert.ok(Number.isInteger(flow.low_reservoir_events_400ms));
    assert.ok(Number.isInteger(flow.low_reservoir_events_200ms));
  } finally { runtime.close(); process.env.SPEAKER_CODEC = saved; }
});
