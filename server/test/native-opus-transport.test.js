import assert from "node:assert/strict";
import test from "node:test";
import { createDecoder } from "libopus-wasm";
import { OpusPlaybackTransport, OPUS_FRAME_PCM_BYTES } from "../src/opus-transport.js";

const pcm = Buffer.alloc(OPUS_FRAME_PCM_BYTES + 222, 7);

test("native Opus transport preserves sequence, final valid PCM count, and cleanup", async () => {
  const transport = new OpusPlaybackTransport({ playbackId: "p", enabled: true });
  const frames = [];
  await transport.begin();
  assert.equal(transport.beginMessage({ type: "speaker_begin" }).codec, "opus");
  await transport.sendPcm(async (data) => frames.push(Buffer.from(data)), pcm);
  await transport.finish(async (data) => frames.push(Buffer.from(data)));
  assert.equal(frames.length, 2);
  assert.equal(frames[0].readUInt16LE(4), 0);
  assert.equal(frames[1].readUInt16LE(4), 1);
  assert.equal(frames[0].readUInt16LE(6), OPUS_FRAME_PCM_BYTES);
  assert.equal(frames[1].readUInt16LE(6), 222);
  const decoder = await createDecoder({ sampleRate: 24_000, channels: 1, maxFrameSize: 960 });
  assert.equal(decoder.decode(frames[0].subarray(8)).length, 960);
  assert.equal(decoder.decode(frames[1].subarray(8)).length, 960);
  decoder.free();
  assert.equal(transport.stats.rawPcmBytes, pcm.length);
  assert.equal(transport.closed, true);
});

test("native Opus transport retains PCM fallback and rejects invalid PCM", async () => {
  const sent = [];
  const transport = new OpusPlaybackTransport({ playbackId: "p", enabled: false });
  assert.equal(transport.beginMessage({ codec: "pcm" }).codec, "pcm");
  await transport.sendPcm(async (data) => sent.push(data), Buffer.alloc(4));
  assert.equal(sent.length, 1);
  const opus = new OpusPlaybackTransport({ playbackId: "p", enabled: true });
  await opus.begin();
  await assert.rejects(() => opus.sendPcm(async () => {}, Buffer.alloc(3)), /invalid_pcm/);
  await opus.free();
});

test("native Opus capability negotiation is explicit", () => {
  assert.equal(OpusPlaybackTransport.supported(new Set(["pcm"])), false);
  assert.equal(OpusPlaybackTransport.supported(new Set(["pcm", "opus"])), true);
});
