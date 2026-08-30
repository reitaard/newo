import assert from "node:assert/strict";
import test from "node:test";
import { speakerCodecFlowState, speakerCreditBytes } from "../src/tts.js";

// Deterministic decoded-PCM event model: WAN timing changes delivery order, not
// credit provenance. Wire bytes are intentionally absent from credit decisions.
function simulate({ bytes, latencyMs, jitterMs = 0, flowDelayMs = 0, cancelAt = null, disconnectAt = null }) {
  let now = 0, produced = bytes, admitted = 0, received = 0, consumed = 0, wire = 0, packets = 0;
  const flight = [], reports = []; const chunk = 1920;
  while ((consumed < bytes || admitted < bytes) && now < 30_000) {
    if (cancelAt !== null && now >= cancelAt) return { cancelled: true, produced, admitted, received, consumed, wire, packets };
    if (disconnectAt !== null && now >= disconnectAt) return { disconnected: true, produced, admitted, received, consumed, wire, packets };
    const buffered = Math.max(0, received - consumed);
    const credit = speakerCreditBytes(admitted, received, consumed, buffered);
    if (admitted < bytes && credit >= Math.min(chunk, bytes - admitted)) {
      const n = Math.min(chunk, bytes - admitted); admitted += n; wire += 80; packets += 1;
      flight.push({ at: now + latencyMs + ((packets % 3) - 1) * jitterMs, bytes: n });
    }
    for (const event of flight.filter((event) => event.at <= now)) { received += event.bytes; reports.push({ at: now + flowDelayMs }); }
    for (let i = flight.length - 1; i >= 0; i--) if (flight[i].at <= now) flight.splice(i, 1);
    consumed += Math.min(Math.max(0, received - consumed), 480); // 10 ms at 48 kB/s
    const state = speakerCodecFlowState({ logicalPcmProduced: produced, wireBytes: wire, opusPackets: packets,
      decodedPcmAdmitted: admitted, decodedPcmReceived: received, decodedPcmConsumed: consumed,
      decodedPcmBuffered: Math.max(0, received - consumed) });
    assert.ok(state.receiverDecodedPcmOutstanding <= 22528, "decoded safety ceiling");
    assert.ok(received <= admitted && admitted <= produced && consumed <= received);
    now += 10;
  }
  assert.ok(now < 30_000, "no deadlock");
  assert.equal(consumed, bytes); assert.equal(received, bytes); assert.equal(admitted, bytes);
  return { produced, admitted, received, consumed, wire, packets, reports: reports.length };
}
for (const [name, options] of Object.entries({ wan20: { latencyMs: 20 }, wan50: { latencyMs: 50 }, wan100: { latencyMs: 100 }, jitter: { latencyMs: 50, jitterMs: 20 }, delayed_flow: { latencyMs: 50, flowDelayMs: 100 }, short_partial: { latencyMs: 20, bytes: 2142 }, long: { latencyMs: 100, bytes: 96_000 } })) test(`codec flow simulation: ${name}`, () => {
  const result = simulate({ bytes: 24_000, ...options }); assert.equal(result.produced, result.consumed); assert.ok(result.wire > 0 && result.packets > 0);
});
test("codec flow simulation cancellation and disconnect make no completion claim", () => {
  assert.equal(simulate({ bytes: 96_000, latencyMs: 100, cancelAt: 40 }).cancelled, true);
  assert.equal(simulate({ bytes: 96_000, latencyMs: 100, disconnectAt: 40 }).disconnected, true);
});
