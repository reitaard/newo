import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import test from "node:test";
import { createSpeakerRuntime } from "../src/tts.js";

class FakeEsp extends EventEmitter {
  constructor(opus) { super(); this.readyState = 1; this.opus = opus; this.sent = []; this.received = 0; this.id = null; this.onResult = null; }
  send(data, options, done) {
    const text = Buffer.isBuffer(data) ? null : String(data); this.sent.push({ data: Buffer.isBuffer(data) ? Buffer.from(data) : text, options });
    if (text?.includes("speaker_begin")) { this.id = JSON.parse(text).playback_id; this.received = 0; }
    if (Buffer.isBuffer(data)) { const valid = this.opus ? data.readUInt16LE(6) : data.length; this.received += valid; setImmediate(() => this.emit("message", Buffer.from(JSON.stringify({ type: "speaker_flow", playback_id: this.id, received_bytes: this.received, consumed_bytes: this.received, buffered_bytes: 0, capacity_bytes: 24576 })), false)); }
    if (text?.includes("speaker_end")) setImmediate(() => this.onResult?.({ type: "speaker_complete", playback_id: this.id, bytes: this.received }));
    done?.();
  }
  close() { this.readyState = 3; this.emit("close"); }
}
const logger = new Proxy({}, { get: () => () => {} });
const backend = { limiter: .95, gainDb: 2, async synthesize() { return Buffer.alloc(2142, 4); } };

for (const opus of [true, false]) test(`native runtime ${opus ? "Opus" : "PCM fallback"} needs no bootstrap`, async () => {
  const saved = process.env.SPEAKER_CODEC; process.env.SPEAKER_CODEC = "opus";
  const ws = new FakeEsp(opus); const runtime = createSpeakerRuntime({ logger, backend, enabled: true, getDevice: () => ({ ws }), sendControl: async () => true, resultTimeoutMs: 1000, flowTimeoutMs: 1000 });
  ws.onResult = (message) => runtime.handleResult("device", message);
  runtime.handleConnection(ws, "device"); ws.emit("message", Buffer.from(JSON.stringify({ type: "speaker_ready", codecs: opus ? ["pcm", "opus"] : ["pcm"] })), false);
  await runtime.speak("Native transport test.").completion;
  await runtime.speak("Native transport second playback.").completion;
  const begins = ws.sent.filter(({ data }) => typeof data === "string" && data.includes("speaker_begin"));
  assert.equal(begins.length, 2, "persistent connection accepts the next playback");
  const begin = JSON.parse(begins[0].data);
  const packets = ws.sent.filter(({ data }) => Buffer.isBuffer(data)).slice(0, opus ? 2 : 2);
  assert.equal(begin.codec ?? "pcm", opus ? "opus" : "pcm");
  if (opus) { assert.equal(begin.opus_frame_ms, 40); assert.equal(begin.opus_bitrate, 24000); assert.equal(packets.length, 2); assert.equal(packets[0].data.subarray(0, 4).toString(), "NWOP"); assert.equal(packets[0].data.readUInt16LE(4), 0); assert.equal(packets[1].data.readUInt16LE(4), 1); assert.equal(packets[1].data.readUInt16LE(6), 222); }
  else assert.equal(packets.reduce((total, packet) => total + packet.data.length, 0), 2142);
  runtime.close(); process.env.SPEAKER_CODEC = saved;
});
