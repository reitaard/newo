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
    commandReply: async (ctx, text, category, requestId, options) => {
      replies.push({ text, category, requestId, options });
      return text;
    },
    commandTrace: () => null,
    getDeviceSnapshot: overrides.getDeviceSnapshot ?? (() => ({ connected: true, status: {} })),
    getSpeakerEnabled: () => speakerEnabled,
    setSpeakerAccepting: (enabled) => { speakerEnabled = enabled; },
    persistSpeakerEnabled: async (enabled) => { speakerEnabled = enabled; return enabled; },
    speakerInfo: { ttsEnabled: true, backend: "kokoro", format: "24 kHz PCM16", bufferBytes: 24_576 },
    getAssistantInfo: overrides.getAssistantInfo ?? (() => ({ status: "ready", model: "helix-qwen3-0.6b", qwen: "online", speakerEnabled, latest: { result: "n/a", llmMs: null, asrFinalMs: null, ttsQueuedMs: null, totalMs: null } })),
  });
  return { handlers, replies, get speakerEnabled() { return speakerEnabled; } };
}

const speakerAck = {
  type: "speaker_ack", request_id: "request-1", enabled: true, connection: "Ready",
  volume: 100, muted: false, applied: true, last_playback: "Complete",
  underruns: 0, overflows: 0, buffer_bytes: 24_576,
};

test("/v sends manual_toggle and returns a terse silent start reply", async () => {
  const requests = [];
  const harness = createHarness((type, responseType, fields) => {
    requests.push({ type, responseType, fields });
    return response({ state: "streaming", voice_connected: false, wake_count: 4, session_count: 12, failures: 0, timeouts: 0 });
  });
  await harness.handlers.voice({ match: "" });
  assert.deepEqual(requests[0], { type: "voice_control", responseType: "voice_ack", fields: { action: "manual_toggle" } });
  assert.equal(harness.replies[0].text, "Listening.");
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
  assert.doesNotMatch(harness.replies[0].text, /Voice:|Trigger:|Sessions:/);
});

test("/v returns a terse silent cancel reply", async () => {
  const harness = createHarness(() => response({ state: "off", voice_connected: false, wake_count: 0, session_count: 1, failures: 0, timeouts: 0 }));
  await harness.handlers.voice({ match: "" });
  assert.equal(harness.replies[0].text, "Stopped.");
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/v reports manual microphone contention tersely and silently", async () => {
  const harness = createHarness(() => response({ state: "off", voice_connected: false, wake_count: 0, session_count: 0, failures: 0, timeouts: 0, applied: false }));
  await harness.handlers.voice({ match: "" });
  assert.equal(harness.replies[0].category, "busy");
  assert.equal(harness.replies[0].text, "Voice busy.");
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/v reports an offline device tersely and silently", async () => {
  const harness = createHarness(() => ({ kind: "offline" }));
  await harness.handlers.voice({ match: "" });
  assert.equal(harness.replies[0].text, "Voice offline.");
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/vs reads bounded assistant telemetry without invoking chat", async () => {
  let telemetryReads = 0;
  const harness = createHarness((type) => {
    assert.equal(type, "voice_status");
    return response({ state: "armed", voice_connected: true, wake_count: 2, session_count: 3, failures: 0, timeouts: 0 });
  }, {
    getAssistantInfo: () => {
      telemetryReads += 1;
      return { status: "error", model: "helix-qwen3-0.6b", qwen: "offline", speakerEnabled: true,
        latest: { result: "timeout", llmMs: 143, asrFinalMs: null, ttsQueuedMs: null, totalMs: null } };
    },
  });
  await harness.handlers.voiceStatus({ match: "" });
  assert.equal(telemetryReads, 1);
  assert.match(harness.replies[0].text, /Assistant: <b>ERROR<\/b>/);
  assert.match(harness.replies[0].text, /LLM: <b>helix-qwen3-0\.6b<\/b>/);
  assert.match(harness.replies[0].text, /Qwen: <b>OFFLINE<\/b>/);
  assert.match(harness.replies[0].text, /Last LLM: <b>143 ms<\/b>/);
  assert.match(harness.replies[0].text, /Last turn: <b>timeout<\/b>/);
  assert.match(harness.replies[0].text, /ASR final: <b>n\/a<\/b>/);
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/vs represents a disabled assistant with clean never values", async () => {
  const harness = createHarness(() => response({ state: "off", voice_connected: false, wake_count: 0, session_count: 0, failures: 0, timeouts: 0 }), {
    getAssistantInfo: () => ({ status: "disabled", model: null, qwen: "disabled", speakerEnabled: false,
      latest: { result: "n/a", llmMs: null, asrFinalMs: null, ttsQueuedMs: null, totalMs: null } }),
  });
  await harness.handlers.voiceStatus({ match: "" });
  assert.match(harness.replies[0].text, /Assistant: <b>DISABLED<\/b>/);
  assert.match(harness.replies[0].text, /LLM: <b>n\/a<\/b>/);
  assert.match(harness.replies[0].text, /Last turn: <b>n\/a<\/b>/);
  assert.match(harness.replies[0].text, /Speaker: <b>OFF<\/b>/);
});

test("/speaker toggles OFF with terse non-spoken confirmation", async () => {
  const harness = createHarness((type, responseType, fields) => {
    assert.equal(type, "speaker_control");
    assert.deepEqual(fields, { action: "set_enabled", enabled: false });
    return response({ ...speakerAck, enabled: false, connection: "Disconnected" });
  });
  await harness.handlers.speaker({ match: "" });
  assert.equal(harness.speakerEnabled, false);
  assert.equal(harness.replies[0].text, "Speaker turned off.");
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/speaker toggles ON with terse non-spoken confirmation", async () => {
  const harness = createHarness((type, responseType, fields) => {
    assert.equal(type, "speaker_control");
    assert.deepEqual(fields, { action: "set_enabled", enabled: true });
    return response({ ...speakerAck, enabled: true, connection: "Ready" });
  }, { speakerEnabled: false });
  await harness.handlers.speaker({ match: "" });
  assert.equal(harness.speakerEnabled, true);
  assert.equal(harness.replies[0].text, "Speaker turned on.");
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
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
