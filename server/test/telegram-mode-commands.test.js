import assert from "node:assert/strict";
import test from "node:test";

import { createPrimaryModeHandlers, parseClockArgument, parseMicArgument, parseTrackArgument, parseUsbArgument, parseVolumeArgument } from "../src/telegram-mode-commands.js";

function response(message) {
  return { kind: "sent", requestId: "request-1", promise: Promise.resolve({ kind: "response", message }) };
}

function createHarness(sendDeviceRequest, overrides = {}) {
  const replies = [];
  const live = [];
  let speakerEnabled = overrides.speakerEnabled ?? true;
  let trackDesired = overrides.trackDesired ?? false;
  const handlers = createPrimaryModeHandlers({
    sendDeviceRequest,
    commandReply: async (ctx, text, category, requestId, options) => {
      replies.push({ text, category, requestId, options });
      return { message_id: replies.length, text };
    },
    commandTrace: () => null,
    getDeviceSnapshot: overrides.getDeviceSnapshot ?? (() => ({ connected: true, status: {} })),
    getSpeakerEnabled: () => speakerEnabled,
    setSpeakerAccepting: (enabled) => { speakerEnabled = enabled; },
    persistSpeakerEnabled: async (enabled) => { speakerEnabled = enabled; return enabled; },
    speakerInfo: { ttsEnabled: true, backend: "kokoro", format: "24 kHz PCM16", bufferBytes: 24_576 },
    getAssistantInfo: overrides.getAssistantInfo ?? (() => ({ status: "ready", provider: "openai_chat", model: "helix-qwen3-0.6b", online: "online", speakerEnabled, latest: { result: "n/a", llmMs: null, asrFinalMs: null, ttsQueuedMs: null, totalMs: null } })),
    setAssistantProfile: overrides.setAssistantProfile,
    getAssistantTuning: overrides.getAssistantTuning,
    setAssistantTuningPreset: overrides.setAssistantTuningPreset,
    setAssistantTuningValue: overrides.setAssistantTuningValue,
    setAssistantSystemPrompt: overrides.setAssistantSystemPrompt,
    ownerVoiceprint: overrides.ownerVoiceprint,
    getTrackDesired: () => trackDesired,
    persistTrackDesired: async (enabled) => { trackDesired = enabled; return enabled; },
    renderTrackSnapshot: ({ debug, transient } = {}) => debug ? "DEBUG PANEL" : transient ? "STARTING PANEL" : "STATUS PANEL",
    startTrackLive: (chatId, messageId, initial) => live.push({ action: "start", chatId, messageId, initial }),
    stopTrackLive: async (chatId) => { live.push({ action: "stop", chatId }); return overrides.hasTrackLive ?? false; },
    hasTrackLive: () => overrides.hasTrackLive ?? false,
  });
  return { handlers, replies, live, get speakerEnabled() { return speakerEnabled; } };
}

const speakerAck = {
  type: "speaker_ack", request_id: "request-1", enabled: true, connection: "Ready",
  volume: 100, muted: false, applied: true, last_playback: "Complete",
  underruns: 0, overflows: 0, buffer_bytes: 24_576,
};

test("/track parser accepts toggle, explicit state, status, and debug", () => {
  assert.deepEqual(parseTrackArgument(""), { kind: "toggle" });
  assert.deepEqual(parseTrackArgument(" ON "), { kind: "on" });
  assert.deepEqual(parseTrackArgument("off"), { kind: "off" });
  assert.deepEqual(parseTrackArgument("status"), { kind: "status" });
  assert.deepEqual(parseTrackArgument("debug"), { kind: "debug" });
  assert.deepEqual(parseTrackArgument("maybe"), { kind: "invalid" });
});

test("/usb aliases resolve real host and granular client controls", () => {
  assert.deepEqual(parseUsbArgument(""), { action: "status" });
  assert.deepEqual(parseUsbArgument("on"), { action: "host", enabled: true });
  assert.deepEqual(parseUsbArgument("a off"), { action: "audio", enabled: false });
  assert.deepEqual(parseUsbArgument("storage_on"), { action: "storage", enabled: true });
  assert.deepEqual(parseUsbArgument("v off"), { action: "vcp", enabled: false });
  assert.equal(parseUsbArgument("fake on"), null);
});

test("/usb sends granular control and renders acknowledged firmware state", async () => {
  const requests = [];
  const harness = createHarness((type, responseType, fields) => {
    requests.push({ type, responseType, fields });
    return response({ type: "usb_ack", host: true, audio: true, storage: false, vcp: false,
      active: false, applied: true, reboot_required: true, trial_pending: true });
  });
  await harness.handlers.usb({ match: "a on" });
  assert.deepEqual(requests[0], { type: "usb_control", responseType: "usb_ack", fields: { action: "audio", enabled: true } });
  assert.match(harness.replies[0].text, /VCP\/Nano/);
  assert.match(harness.replies[0].text, /REBOOTING/);
});

test("/mic aliases select one real processing path and report metrics", async () => {
  assert.deepEqual(parseMicArgument("raw"), { action: "set", mode: "raw", ns_level: 1 });
  assert.deepEqual(parseMicArgument("mild"), { action: "set", mode: "ns", ns_level: 0 });
  assert.deepEqual(parseMicArgument("3"), { action: "set", mode: "ns", ns_level: 2 });
  assert.equal(parseMicArgument("agc"), null);
  const requests = [];
  const harness = createHarness((type, responseType, fields) => {
    requests.push({ type, responseType, fields });
    return response({ type: "mic_ack", mode: "ns", ns_level: 2, applied: true,
      raw_rms: 120, clean_rms: 80, raw_peak: 900, clean_peak: 700,
      raw_clipped: 0, clean_clipped: 0, noise_floor_rms: 12 });
  });
  await harness.handlers.mic({ match: "strong" });
  assert.deepEqual(requests[0], { type: "mic_control", responseType: "mic_ack",
    fields: { action: "set", mode: "ns", ns_level: 2 } });
  assert.match(harness.replies[0].text, /STRONG/);
  assert.match(harness.replies[0].text, /Noise floor/);
});

test("/track starts one live panel and persists desired state", async () => {
  const requests = [];
  const harness = createHarness((type, responseType, fields) => {
    requests.push({ type, responseType, fields });
    return response({ type: "track_ack", state: "active", applied: true });
  });
  await harness.handlers.track({ match: "on", chat: { id: 42 } });
  assert.deepEqual(requests[0], { type: "track_control", responseType: "track_ack", fields: { action: "on" } });
  assert.equal(harness.replies[0].text, "STARTING PANEL");
  assert.deepEqual(harness.live, [{ action: "start", chatId: 42, messageId: 1, initial: "STARTING PANEL" }]);
});

test("/track status and debug are read-only snapshots", async () => {
  const harness = createHarness(() => response({ type: "track_ack", state: "active", applied: true }));
  await harness.handlers.track({ match: "status", chat: { id: 1 } });
  await harness.handlers.track({ match: "debug", chat: { id: 1 } });
  assert.deepEqual(harness.replies.map((item) => item.text), ["STATUS PANEL", "DEBUG PANEL"]);
});

test("/track_bg moves an active live panel to background without a device command", async () => {
  let requests = 0;
  const harness = createHarness(() => { requests += 1; return response({ state: "active", applied: true }); },
                                { trackDesired: true, hasTrackLive: true });
  await harness.handlers.trackBackground({ match: "", chat: { id: 8 } });
  assert.equal(requests, 0);
  assert.deepEqual(harness.live, [{ action: "stop", chatId: 8 }]);
  assert.equal(harness.replies[0].text, "Tracking in background…");
});

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

test("owner enrollment exposes real three-sample state and cancellation", async () => {
  let active = false;
  const ownerVoiceprint = {
    begin() { active = true; return { enabled: true, enrolled: false, enrollment_active: true, enrollment_samples: 0, enrollment_required: 3, threshold: 0.65 }; },
    status() { return { enabled: true, enrolled: false, enrollment_active: active, enrollment_samples: 0, enrollment_required: 3, threshold: 0.65 }; },
    cancel() { const changed = active; active = false; return changed; },
  };
  const harness = createHarness(() => ({ kind: "offline" }), { ownerVoiceprint });
  await harness.handlers.ownerEnroll({});
  assert.match(harness.replies[0].text, /0\/3/);
  assert.match(harness.replies[0].text, /Hi Wall-E/);
  await harness.handlers.ownerCancel({});
  assert.equal(harness.replies[1].text, "Owner enrollment cancelled.");
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
      return { status: "error", provider: "openai_chat", model: "helix-qwen3-0.6b", online: "offline", speakerEnabled: true,
        latest: { result: "timeout", llmMs: 143, asrFinalMs: null, ttsQueuedMs: null, totalMs: null } };
    },
  });
  await harness.handlers.voiceStatus({ match: "" });
  assert.equal(telemetryReads, 1);
  assert.match(harness.replies[0].text, /Assistant: <b>ERROR<\/b>/);
  assert.match(harness.replies[0].text, /LLM: <b>helix-qwen3-0\.6b<\/b>/);
  assert.match(harness.replies[0].text, /Provider: <b>openai_chat<\/b>/);
  assert.match(harness.replies[0].text, /Model state: <b>OFFLINE<\/b>/);
  assert.match(harness.replies[0].text, /Last LLM: <b>143 ms<\/b>/);
  assert.match(harness.replies[0].text, /Last turn: <b>timeout<\/b>/);
  assert.match(harness.replies[0].text, /ASR final: <b>n\/a<\/b>/);
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/vs represents a disabled assistant with clean never values", async () => {
  const harness = createHarness(() => response({ state: "off", voice_connected: false, wake_count: 0, session_count: 0, failures: 0, timeouts: 0 }), {
    getAssistantInfo: () => ({ status: "disabled", provider: "openai_chat", model: null, online: "disabled", speakerEnabled: false,
      latest: { result: "n/a", llmMs: null, asrFinalMs: null, ttsQueuedMs: null, totalMs: null } }),
  });
  await harness.handlers.voiceStatus({ match: "" });
  assert.match(harness.replies[0].text, /Assistant: <b>DISABLED<\/b>/);
  assert.match(harness.replies[0].text, /LLM: <b>n\/a<\/b>/);
  assert.match(harness.replies[0].text, /Last turn: <b>n\/a<\/b>/);
  assert.match(harness.replies[0].text, /Speaker: <b>OFF<\/b>/);
});

test("/profile reports preferred and effective profile compactly", async () => {
  const harness = createHarness(() => ({ kind: "offline" }), {
    getAssistantInfo: () => ({ preferred_profile: "lfm2.5:8b", effective_profile: "qwen3:0.6b",
      provider: "openai_chat", model: "helix-qwen3-0.6b", online: "online",
      fallback_active: true, fallback_reason: "assistant_timeout" }),
  });
  await harness.handlers.profile({ match: "" });
  assert.match(harness.replies[0].text, /Preferred: <b>lfm2\.5:8b<\/b>/);
  assert.match(harness.replies[0].text, /Effective: <b>qwen3:0\.6b<\/b>/);
  assert.match(harness.replies[0].text, /Fallback: <b>ON \(assistant_timeout\)<\/b>/);
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/profile aliases and clickable underscore commands switch immediately", async () => {
  const selected = [];
  const setAssistantProfile = async (value) => {
    selected.push(value);
    return { preferred_profile: value === "qwen" ? "qwen3:0.6b" : "lfm2.5:8b",
      effective_profile: value === "qwen" ? "qwen3:0.6b" : "lfm2.5:8b",
      provider: value === "qwen" ? "openai_chat" : "ollama_raw",
      model: value === "qwen" ? "helix-qwen3-0.6b" : "newo-main", online: "online", fallback_active: false };
  };
  const harness = createHarness(() => ({ kind: "offline" }), { setAssistantProfile });
  await harness.handlers.profile({ match: "lfm" });
  await harness.handlers.profile({ match: "qwen" });
  await harness.handlers.profile({ match: "" }, "lfm");
  await harness.handlers.profile({ match: "" }, "qwen");
  assert.deepEqual(selected, ["lfm", "qwen", "lfm", "qwen"]);
  assert.ok(harness.replies.every((reply) => reply.options.newoSpeak === false));
});

test("/pt shows and applies compact persistent tuning presets", async () => {
  const selected = [];
  const tuning = { id: "lfm2.5:8b", temperature: 0.2, top_k: 80, repeat_penalty: 1.05, max_tokens: 64, max_chars: 300, timeout_ms: 15_000 };
  const harness = createHarness(() => ({ kind: "offline" }), {
    getAssistantTuning: () => tuning,
    setAssistantTuningPreset: async (preset) => { selected.push(preset); return { ...tuning, max_tokens: 48, max_chars: 240, timeout_ms: 10_000 }; },
  });
  await harness.handlers.profileTune({ match: "" });
  await harness.handlers.profileTune({ match: "fast" });
  assert.match(harness.replies[0].text, /64 tokens \/ 300 chars/);
  assert.match(harness.replies[1].text, /48 tokens \/ 240 chars/);
  assert.deepEqual(selected, ["fast"]);
});

test("/p hidden direct tuning accepts short aliases and rejects invalid values", async () => {
  const selected = [];
  const tuning = { id: "lfm2.5:8b", temperature: 0.2, top_k: 80, repeat_penalty: 1.05, max_tokens: 64, max_chars: 300, timeout_ms: 15_000 };
  const harness = createHarness(() => ({ kind: "offline" }), {
    setAssistantTuningValue: async (key, value) => { selected.push([key, value]); return tuning; },
  });
  await harness.handlers.profile({ match: "s topk 60" });
  await harness.handlers.profile({ match: "set maxchars 280" });
  assert.deepEqual(selected, [["topk", "60"], ["maxchars", "280"]]);
  assert.ok(harness.replies.every((reply) => reply.options.newoSpeak === false));
});

test("/p s sysprompt accepts the next message or /cancel", async () => {
  const saved = [];
  const tuning = { id: "lfm2.5:8b", temperature: 0.2, top_k: 80, repeat_penalty: 1.05, max_tokens: 64, max_chars: 300, timeout_ms: 15_000, system_prompt: "New prompt." };
  const harness = createHarness(() => ({ kind: "offline" }), {
    setAssistantSystemPrompt: async (prompt) => { saved.push(prompt); return tuning; },
  });
  const ctx = { match: "s sysprompt", chat: { id: 7 }, from: { id: 9 } };
  await harness.handlers.profile(ctx);
  assert.match(harness.replies[0].text, /Send the new system prompt now, or use \/cancel/);
  assert.equal(await harness.handlers.profilePromptInput({ ...ctx, message: { text: "New prompt." } }), true);
  assert.deepEqual(saved, ["New prompt."]);
  assert.match(harness.replies[1].text, /<i>System prompt:<\/i>\n<blockquote>New prompt\.<\/blockquote>/);
  await harness.handlers.profile(ctx);
  await harness.handlers.cancelProfilePrompt(ctx);
  assert.equal(await harness.handlers.profilePromptInput({ ...ctx, message: { text: "Must not save." } }), false);
  assert.deepEqual(saved, ["New prompt."]);
});

test("/speaker toggles OFF with terse non-spoken confirmation", async () => {
  const harness = createHarness((type, responseType, fields) => {
    assert.equal(type, "speaker_control");
    assert.deepEqual(fields, { action: "set_enabled", enabled: false, led_feedback: true });
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
    assert.deepEqual(fields, { action: "set_enabled", enabled: true, led_feedback: true });
    return response({ ...speakerAck, enabled: true, connection: "Ready" });
  }, { speakerEnabled: false });
  await harness.handlers.speaker({ match: "" });
  assert.equal(harness.speakerEnabled, true);
  assert.equal(harness.replies[0].text, "Speaker turned on.");
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/speaker does not claim success until firmware confirms the requested state", async () => {
  const harness = createHarness(() => response({ ...speakerAck, enabled: true, connection: "Connecting", applied: false }),
    { speakerEnabled: false });
  await harness.handlers.speaker({ match: "" });
  assert.equal(harness.replies[0].text, "Speaker change was not confirmed.");
  assert.equal(harness.replies[0].category, "device_error");
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/speaker accepts an applied ON state whose connection is deferred behind WakeNet", async () => {
  const harness = createHarness(() => response({ ...speakerAck, enabled: true, connection: "Connecting", applied: true }),
    { speakerEnabled: false });
  await harness.handlers.speaker({ match: "" });
  assert.equal(harness.replies[0].text, "Speaker turned on.");
  assert.equal(harness.replies[0].options.newoSpeak, false);
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
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/clock toggles, sets, reports status, and rejects malformed input after firmware ACK", async () => {
  assert.deepEqual(parseClockArgument(""), { kind: "toggle" });
  assert.deepEqual(parseClockArgument("ON"), { kind: "on" });
  assert.deepEqual(parseClockArgument("off"), { kind: "off" });
  assert.deepEqual(parseClockArgument("status"), { kind: "status" });
  assert.deepEqual(parseClockArgument("later"), { kind: "invalid" });

  const calls = [];
  const enabledByAction = { toggle: false, on: true, off: false, status: false };
  const harness = createHarness((type, responseType, fields) => {
    calls.push({ type, responseType, fields });
    return response({ enabled: enabledByAction[fields.action], applied: true });
  });
  await harness.handlers.clock({ match: "" });
  await harness.handlers.clock({ match: "on" });
  await harness.handlers.clock({ match: "off" });
  await harness.handlers.clock({ match: "status" });
  await harness.handlers.clock({ match: "bad" });
  assert.deepEqual(calls, [
    { type: "clock_control", responseType: "clock_ack", fields: { action: "toggle" } },
    { type: "clock_control", responseType: "clock_ack", fields: { action: "on" } },
    { type: "clock_control", responseType: "clock_ack", fields: { action: "off" } },
    { type: "clock_control", responseType: "clock_ack", fields: { action: "status" } },
  ]);
  assert.deepEqual(harness.replies.slice(0, 4).map((reply) => reply.text),
                   ["Clock OFF.", "Clock ON.", "Clock OFF.", "Clock OFF."]);
  assert.equal(harness.replies[4].text, "Usage: /clock [on|off|status]");
  for (const reply of harness.replies) assert.deepEqual(reply.options, { newoSpeak: false });
});

test("/clock does not claim success without a firmware ACK", async () => {
  const harness = createHarness(() => response({ enabled: true, applied: false }));
  await harness.handlers.clock({ match: "on" });
  assert.equal(harness.replies[0].text, "Clock unavailable.");
  assert.equal(harness.replies[0].category, "response");
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});

test("/clock reports an offline device without issuing a control request", async () => {
  const harness = createHarness(() => ({ kind: "offline" }));
  await harness.handlers.clock({ match: "status" });
  assert.equal(harness.replies[0].text, "Clock offline.");
  assert.equal(harness.replies[0].category, "offline");
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
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
  for (const reply of harness.replies) assert.deepEqual(reply.options, { newoSpeak: false });
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
  assert.deepEqual(harness.replies[0].options, { newoSpeak: false });
});
