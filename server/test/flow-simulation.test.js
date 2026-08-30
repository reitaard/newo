import assert from "node:assert/strict";
import test from "node:test";
import { speakerCodecFlowState, speakerCreditBytes } from "../src/tts.js";

// Deterministic 10-ms simulation. Sender sees only delayed speaker_flow reports.
function simulate({ bytes = 24_000, latencyMs = 50, jitterMs = 0, flowDelayMs = 0, cancelAt, disconnectAt, noFlow = false, producerStallAt }) {
  let now = 0, admitted = 0, wire = 0, packets = 0, stalls = 0;
  let actualReceived = 0, actualConsumed = 0, actualBuffered = 0;
  let reportedReceived = 0, reportedConsumed = 0, reportedBuffered = 0;
  const flight = [], reports = []; const chunk = 1920;
  while ((actualConsumed < bytes || admitted < bytes) && now < 30_000) {
    if (cancelAt !== undefined && now >= cancelAt) return { cancelled: true, admitted, actualReceived, actualConsumed, stalls };
    if (disconnectAt !== undefined && now >= disconnectAt) return { disconnected: true, admitted, actualReceived, actualConsumed, stalls };
    for (let i = flight.length - 1; i >= 0; i--) if (flight[i].at <= now) { actualReceived += flight[i].bytes; actualBuffered += flight[i].bytes; if (!noFlow) reports.push({ at: now + flowDelayMs, received: actualReceived, consumed: actualConsumed, buffered: actualBuffered }); flight.splice(i, 1); }
    actualBuffered -= Math.min(actualBuffered, 480); actualConsumed += Math.min(480, actualReceived - actualConsumed);
    if (!noFlow) reports.push({ at: now + flowDelayMs, received: actualReceived, consumed: actualConsumed, buffered: actualBuffered });
    for (let i = reports.length - 1; i >= 0; i--) if (reports[i].at <= now) { ({ received: reportedReceived, consumed: reportedConsumed, buffered: reportedBuffered } = reports[i]); reports.splice(i, 1); }
    const credit = speakerCreditBytes(admitted, reportedReceived, reportedConsumed, reportedBuffered);
    const canProduce = producerStallAt === undefined || now < producerStallAt || now > producerStallAt + 100;
    if (admitted < bytes && canProduce && credit >= Math.min(chunk, bytes - admitted)) { const n = Math.min(chunk, bytes - admitted); admitted += n; wire += 80; packets++; flight.push({ at: now + latencyMs + ((packets % 3) - 1) * jitterMs, bytes: n }); } else if (admitted < bytes && canProduce) stalls++;
    assert.ok(actualReceived <= admitted && admitted <= bytes && actualConsumed <= actualReceived);
    const state = speakerCodecFlowState({ logicalPcmProduced: bytes, wireBytes: wire, opusPackets: packets, decodedPcmAdmitted: admitted, decodedPcmReceived: actualReceived, decodedPcmConsumed: actualConsumed, decodedPcmBuffered: actualBuffered });
    assert.ok(state.receiverDecodedPcmOutstanding <= 22528, "decoded safety ceiling; wire is never credit");
    now += 10;
  }
  if (noFlow) return { timedOut: now >= 30_000 || stalls > 0, admitted, stalls };
  assert.ok(now < 30_000, "no deadlock"); assert.equal(actualConsumed, bytes); assert.equal(actualReceived, bytes); assert.equal(admitted, bytes);
  return { admitted, actualReceived, actualConsumed, wire, packets, stalls, elapsed: now };
}
for (const [name, options] of Object.entries({ wan20: { latencyMs: 20 }, wan50: { latencyMs: 50 }, wan100: { latencyMs: 100 }, jitter_burst: { latencyMs: 50, jitterMs: 30 }, delayed100: { flowDelayMs: 100 }, delayed250: { flowDelayMs: 250 }, delayed500: { flowDelayMs: 500 }, producer_stall: { producerStallAt: 100 }, short_partial: { bytes: 2142 }, long: { bytes: 96_000 } })) test(`codec flow simulation: ${name}`, () => {
  const r = simulate(options); assert.ok(r.wire > 0 && r.packets > 0);
});
test("delayed reports materially stall sender credit", () => { const immediate = simulate({ bytes: 96_000, flowDelayMs: 0 }); const delayed = simulate({ bytes: 96_000, flowDelayMs: 500 }); assert.ok(delayed.elapsed > immediate.elapsed || delayed.stalls > immediate.stalls); });
test("cancellation, disconnect, and no-flow never fake completion", () => { assert.equal(simulate({ bytes: 96_000, cancelAt: 40 }).cancelled, true); assert.equal(simulate({ bytes: 96_000, disconnectAt: 40 }).disconnected, true); assert.equal(simulate({ bytes: 96_000, noFlow: true }).timedOut, true); });
