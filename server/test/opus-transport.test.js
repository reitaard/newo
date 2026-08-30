import assert from "node:assert/strict";
import { once } from "node:events";
import test from "node:test";

import WebSocket, { WebSocketServer } from "ws";
import { createDecoder } from "libopus-wasm";

process.env.SPEAKER_CODEC = "opus";
await import("../src/opus-bootstrap.js");

function send(ws, data, options) {
  return new Promise((resolve, reject) => {
    const done = (error) => error ? reject(error) : resolve();
    if (options === undefined) ws.send(data, done);
    else ws.send(data, options, done);
  });
}

test("Opus transport negotiates, frames, decodes, and preserves exact PCM length", async () => {
  const wss = new WebSocketServer({ host: "127.0.0.1", port: 0, perMessageDeflate: false });
  await once(wss, "listening");
  const address = wss.address();
  assert.equal(typeof address, "object");

  const serverConnected = once(wss, "connection");
  const client = new WebSocket(`ws://127.0.0.1:${address.port}`);
  await once(client, "open");
  const [server] = await serverConnected;

  const decoder = await createDecoder({ sampleRate: 24_000, channels: 1, maxFrameSize: 960 });
  let begin = null;
  let end = null;
  let nextSequence = 0;
  let validDecodedBytes = 0;
  let wireBytes = 0;
  let packetCount = 0;
  let resolveEnd;
  const endSeen = new Promise((resolve) => { resolveEnd = resolve; });

  client.on("message", (data, isBinary) => {
    if (!isBinary) {
      const message = JSON.parse(data.toString("utf8"));
      if (message.type === "speaker_begin") begin = message;
      if (message.type === "speaker_end") { end = message; resolveEnd(); }
      return;
    }

    const envelope = Buffer.from(data);
    assert.equal(envelope.subarray(0, 4).toString("ascii"), "NWOP");
    assert.equal(envelope.readUInt16LE(4), nextSequence++);
    const validPcmBytes = envelope.readUInt16LE(6);
    assert.ok(validPcmBytes > 0 && validPcmBytes <= 1_920 && (validPcmBytes & 1) === 0);
    const decoded = decoder.decode(envelope.subarray(8));
    assert.equal(decoded.length, 960);
    validDecodedBytes += validPcmBytes;
    wireBytes += envelope.length;
    packetCount += 1;
  });

  const serverReady = once(server, "message");
  client.send(JSON.stringify({ type: "speaker_ready", codecs: ["pcm", "opus"] }));
  await serverReady;

  const playbackId = "00000000-0000-4000-8000-000000000001";
  const pcm = Buffer.alloc(74_880);
  for (let i = 0; i < pcm.length / 2; i += 1) {
    pcm.writeInt16LE(Math.round(Math.sin(2 * Math.PI * 440 * i / 24_000) * 10_000), i * 2);
  }

  await send(server, JSON.stringify({
    type: "speaker_begin", playback_id: playbackId, sample_rate: 24_000,
    channels: 1, bits_per_sample: 16, streaming: true, max_bytes: 2_880_000,
  }));
  for (let offset = 0; offset < pcm.length; offset += 2_048) {
    await send(server, pcm.subarray(offset, Math.min(offset + 2_048, pcm.length)), { binary: true });
  }
  await send(server, JSON.stringify({ type: "speaker_end", playback_id: playbackId, bytes: pcm.length }));
  await endSeen;

  assert.equal(begin?.codec, "opus");
  assert.equal(begin?.opus_frame_ms, 40);
  assert.equal(begin?.opus_frame_pcm_bytes, 1_920);
  assert.equal(begin?.opus_bitrate, 24_000);
  assert.equal(end?.bytes, pcm.length);
  assert.equal(validDecodedBytes, pcm.length);
  assert.ok(packetCount > 1);
  assert.ok(wireBytes < pcm.length / 5, `expected strong compression, got ${wireBytes}/${pcm.length}`);

  decoder.free();
  client.close();
  await once(client, "close");
  await new Promise((resolve) => wss.close(resolve));
});
