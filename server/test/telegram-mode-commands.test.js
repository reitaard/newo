import assert from "node:assert/strict";
import test from "node:test";

import { createPrimaryModeHandlers, parseVolumeArgument } from "../src/telegram-mode-commands.js";

function response(message) {
  return { kind: "sent", requestId: "request-1", promise: Promise.resolve({ kind: "response", message }) };
}

function createHarness(sendDeviceRequest, overrides = {}) {
  const replies = [];
  let speakerEnabled = overrides.speakerEnabled ?? true;
  const handlers = createPrimaryModeHandlers({
    sendDeviceRequest,
    commandReply: async (ctx, text, category) => { replies.push({ text, category }); return text; },
    commandTrace: () => null,
    getDeviceSnapshot: overrides.getDeviceSnapshot ?? (() => ({ connected: true, status: {} })),
    getSpeakerEnabled: () => speakerEnabled,
    setSpeakerAccepting: (enabled) => { speakerEnabled = enabled; },
    persistSpeakerEnabled: async (enabled) => { speakerEnabled = enabled; return enabled; },
    speakerInfo: { ttsEnabled: true, backend: "espeak", format: "16 kHz PCM16", bufferBytes: 16_384 },
  });
  return { handlers, replies, get speakerEnabled() { return speakerEnabled; } };
}

const speakerAck = {
  type: "speaker_ack", request_id: "request-1", enabled: true, connection: "Ready",
  volume: 100, muted: false, applied: true, last_playback: "Complete",
  underruns: 0, overflows: 0, buffer_bytes: 16_384,
};

test("/voice toggles and returns detailed current status", async () => {
  const requests = [];
  const harness = createHarness((type, responseType, fields) => {
    requests.push({ type, responseType, fields });
    return response({ state: "armed", voice_connected: true, wake_count: 4, session_count: 12, failures: 0, timeouts: 0 });
  });
  await harness.handlers.voice({ match: "" });
  assert.deepEqual(requests[0], { type: "voice_control", responseType: "voice_ack", fields: { action: "toggle" } });
  assert.match(harness.replies[0].text, /Voice: <b>ARMED<\/b>/);
  assert.match(harness.replies[0].text, /WakeNet: <b>Ready<\/b>/);
  assert.match(harness.replies[0].text, /Sessions: <b>12<\/b>/);
  assert.match(harness.replies[0].text, /Timeouts: <b>0<\/b>/);
});

test("/speaker toggles persisted automatic replies and returns detailed status", async () => {
  const harness = createHarness((type, responseType, fields) => {
    assert.equal(type, "speaker_control");
    assert.deepEqual(fields, { action: "set_enabled", enabled: false });
    return response({ ...speakerAck, enabled: false, connection: "Disconnected" });
  });
  await harness.handlers.speaker({ match: "" });
  assert.equal(harness.speakerEnabled, false);
  assert.match(harness.replies[0].text, /Speaker: <b>OFF<\/b>/);
  assert.match(harness.replies[0].text, /Connection: <b>Disconnected<\/b>/);
  assert.match(harness.replies[0].text, /Volume: <b>100%<\/b>/);
  assert.match(harness.replies[0].text, /Buffer: <b>16384 bytes<\/b>/);
  assert.match(harness.replies[0].text, /Overflows: <b>0<\/b>/);
});

test("/eco toggles then refreshes detailed device telemetry", async () => {
  const calls = [];
  const snapshot = { connected: true, status: { ssid: "lab", rssi: -56, uptime_ms: 65_000, free_heap: 204_800, free_psram: 4_194_304 } };
  const harness = createHarness((type) => {
    calls.push(type);
    return type === "eco_toggle" ? response({ mode: "eco_on" }) : response(snapshot.status);
  }, { getDeviceSnapshot: () => snapshot });
  await harness.handlers.eco({ match: "" });
  assert.deepEqual(calls, ["eco_toggle", "status_request"]);
  assert.match(harness.replies[0].text, /ECO: <b>ON<\/b>/);
  assert.match(harness.replies[0].text, /RSSI: <b>-56 dBm<\/b>/);
  assert.match(harness.replies[0].text, /PSRAM: <b>4\.00 MB<\/b>/);
});

test("/volume reads, sets, and rejects invalid values", async () => {
  assert.deepEqual(parseVolumeArgument(""), { kind: "read" });
  assert.deepEqual(parseVolumeArgument("0"), { kind: "set", volume: 0 });
  assert.deepEqual(parseVolumeArgument("100"), { kind: "set", volume: 100 });
  assert.deepEqual(parseVolumeArgument("101"), { kind: "invalid" });
  assert.deepEqual(parseVolumeArgument("12.5"), { kind: "invalid" });

  const calls = [];
  const harness = createHarness((type, responseType, fields) => {
    calls.push({ type, responseType, fields });
    return response({ ...speakerAck, volume: fields.volume ?? 100 });
  });
  await harness.handlers.volume({ match: "" });
  await harness.handlers.volume({ match: "65" });
  await harness.handlers.volume({ match: "101" });
  assert.equal(calls[0].type, "speaker_status");
  assert.deepEqual(calls[1].fields, { action: "set_volume", volume: 65 });
  assert.match(harness.replies[1].text, /Volume: <b>65%<\/b>/);
  assert.equal(harness.replies[2].category, "usage");
});

test("/mute toggles and reports mute plus retained volume", async () => {
  const harness = createHarness((type, responseType, fields) => {
    assert.equal(type, "speaker_control");
    assert.deepEqual(fields, { action: "toggle_mute" });
    return response({ ...speakerAck, volume: 70, muted: true });
  });
  await harness.handlers.mute({ match: "" });
  assert.match(harness.replies[0].text, /Mute: <b>ON<\/b>/);
  assert.match(harness.replies[0].text, /Volume: <b>70%<\/b>/);
});
