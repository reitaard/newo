const escapeHtml = (value) => String(value ?? "").replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;");
const bold = (value) => `<b>${escapeHtml(value)}</b>`;
const italic = (value) => `<i>${escapeHtml(value)}</i>`;
const title = (value) => `<b><i>${escapeHtml(value)}</i></b>`;
const quote = (lines) => `<blockquote>${lines.join("\n")}</blockquote>`;
const message = (name, lines) => `${title(`${name}:`)}\n${quote(lines)}`;

function duration(milliseconds) {
  if (!Number.isFinite(milliseconds) || milliseconds < 0) return "unknown";
  let seconds = Math.floor(milliseconds / 1_000);
  const days = Math.floor(seconds / 86_400); seconds %= 86_400;
  const hours = Math.floor(seconds / 3_600); seconds %= 3_600;
  const minutes = Math.floor(seconds / 60); seconds %= 60;
  const parts = [];
  if (days) parts.push(`${days}d`);
  if (hours) parts.push(`${hours}h`);
  if (minutes) parts.push(`${minutes}m`);
  if (seconds || parts.length === 0) parts.push(`${seconds}s`);
  return parts.join(" ");
}

function bytes(value) {
  if (!Number.isFinite(value) || value < 0) return "unknown";
  if (value >= 1024 * 1024) return `${(value / 1024 / 1024).toFixed(2)} MB`;
  return `${Math.round(value / 1024)} KB`;
}

function timing(value) { return Number.isFinite(value) && value >= 0 ? `${Math.round(value)} ms` : "n/a"; }

export function formatVoiceStatus(voice, assistant = {}) {
  const state = String(voice.state ?? "off").toUpperCase();
  const latest = assistant.latest ?? {};
  return message("voice", [
    `Voice: ${bold(state)}`,
    `Trigger: ${bold("Manual (/v)")}`,
    `Wake word: ${bold("Deferred")}`,
    `Assistant: ${bold(String(assistant.status ?? "disabled").toUpperCase())}`,
    `LLM: ${bold(assistant.model ?? "n/a")}`,
    `Qwen: ${bold(String(assistant.qwen ?? "n/a").toUpperCase())}`,
    `Last LLM: ${bold(timing(latest.llmMs))}`,
    `Last turn: ${bold(latest.result ?? "n/a")}`,
    `ASR final: ${bold(timing(latest.asrFinalMs))}`,
    `TTS queued: ${bold(timing(latest.ttsQueuedMs))}`,
    `Total: ${bold(timing(latest.totalMs))}`,
    `Speaker: ${bold(assistant.speakerEnabled ? "ON" : "OFF")}`,
    `Cloud voice: ${bold(voice.voice_connected ? "Connected" : "Disconnected")}`,
    `Wake count: ${bold(voice.wake_count ?? 0)}`,
    `Sessions: ${bold(voice.session_count ?? 0)}`,
    `Failures: ${bold(voice.failures ?? "Unavailable")}`,
    `Timeouts: ${bold(voice.timeouts ?? "Unavailable")}`,
  ]);
}

export function formatSpeakerStatus({ enabled, ttsEnabled, backend, format, bufferBytes, device }) {
  return message("speaker", [
    `Speaker: ${bold(enabled ? "ON" : "OFF")}`,
    `Connection: ${bold(device?.connection ?? "Disconnected")}`,
    `TTS: ${bold(ttsEnabled ? "Enabled" : "Disabled")}`,
    `Volume: ${bold(device ? `${device.volume}%` : "Unavailable")}`,
    `Mute: ${bold(device ? (device.muted ? "ON" : "OFF") : "Unavailable")}`,
    `Backend: ${bold(backend)}`,
    `Format: ${bold(format)}`,
    `Buffer: ${bold(`${device?.buffer_bytes ?? bufferBytes} bytes`)}`,
    `Last playback: ${bold(device?.last_playback ?? "Unavailable")}`,
    `Underruns: ${bold(device?.underruns ?? "Unavailable")}`,
    `Overflows: ${bold(device?.overflows ?? "Unavailable")}`,
  ]);
}

export function formatEcoStatus(enabled, snapshot) {
  const status = snapshot.status ?? {};
  return message("eco", [
    `ECO: ${bold(enabled ? "ON" : "OFF")}`,
    `Page rotation: ${bold("5s")}`,
    `WiFi: ${bold(snapshot.connected && status.ssid ? "Connected" : "Disconnected")}`,
    `Cloud: ${bold(snapshot.connected ? "Connected" : "Disconnected")}`,
    `RSSI: ${bold(typeof status.rssi === "number" ? `${status.rssi} dBm` : "Unavailable")}`,
    `Uptime: ${bold(duration(status.uptime_ms))}`,
    `Heap: ${bold(bytes(status.free_heap))}`,
    `PSRAM: ${bold(bytes(status.free_psram))}`,
  ]);
}

export function formatVolumeStatus(device) {
  return message("volume", [
    `Volume: ${bold(`${device.volume}%`)}`,
    `Mute: ${bold(device.muted ? "ON" : "OFF")}`,
  ]);
}

export function formatMuteStatus(device) {
  return message("mute", [
    `Mute: ${bold(device.muted ? "ON" : "OFF")}`,
    `Volume: ${bold(`${device.volume}%`)}`,
  ]);
}

export function parseVolumeArgument(match) {
  const input = String(match ?? "").trim();
  if (!input) return { kind: "read" };
  if (!/^\d{1,3}$/.test(input)) return { kind: "invalid" };
  const volume = Number(input);
  return volume <= 100 ? { kind: "set", volume } : { kind: "invalid" };
}

export function createPrimaryModeHandlers({
  sendDeviceRequest,
  commandReply,
  commandTrace,
  getDeviceSnapshot,
  getSpeakerEnabled,
  setSpeakerAccepting,
  persistSpeakerEnabled,
  speakerInfo,
  getAssistantInfo = () => ({}),
}) {
  const unavailable = (name, status) => message(name, [`Status: ${bold(status)}`]);

  async function requestSpeakerStatus(ctx, action = null, fields = {}) {
    const request = action
      ? sendDeviceRequest("speaker_control", "speaker_ack", { action, ...fields }, commandTrace(ctx))
      : sendDeviceRequest("speaker_status", "speaker_ack", {}, commandTrace(ctx));
    if (request.kind !== "sent") return { request, device: null };
    const result = await request.promise;
    return { request, result, device: result.kind === "response" ? result.message : null };
  }

  async function voice(ctx) {
    if (String(ctx.match ?? "").trim()) return commandReply(ctx, "Usage: /voice", "usage", null, { newoSpeak: false });
    const request = sendDeviceRequest("voice_control", "voice_ack", { action: "manual_toggle" }, commandTrace(ctx));
    if (request.kind !== "sent") return commandReply(ctx, "Voice offline.", "offline", null, { newoSpeak: false });
    const result = await request.promise;
    if (result.kind === "response" && result.message.applied === false) return commandReply(ctx, "Voice busy.", "busy", request.requestId, { newoSpeak: false });
    if (result.kind === "response") {
      const text = result.message.state === "streaming" ? "Listening." : "Stopped.";
      return commandReply(ctx, text, "response", request.requestId, { newoSpeak: false });
    }
    return commandReply(ctx, "Voice offline.", result.kind, request.requestId, { newoSpeak: false });
  }

  async function voiceStatus(ctx) {
    const request = sendDeviceRequest("voice_status", "voice_ack", {}, commandTrace(ctx));
    if (request.kind !== "sent") return commandReply(ctx, unavailable("voice", "offline"), "offline", null, { newoSpeak: false });
    const result = await request.promise;
    if (result.kind === "response") return commandReply(ctx, formatVoiceStatus(result.message, getAssistantInfo()), "response", request.requestId, { newoSpeak: false });
    return commandReply(ctx, unavailable("voice", result.kind === "timeout" ? "No reply" : "offline"), result.kind, request.requestId, { newoSpeak: false });
  }

  let speakerToggleQueue = Promise.resolve();

  async function applySpeakerToggle(ctx) {
    const enabled = !getSpeakerEnabled();
    // ON is durable before asking the device to connect. OFF stops accepting
    // automatic work immediately, then becomes durable after teardown is acked.
    if (enabled) {
      try { await persistSpeakerEnabled(true); }
      catch { return commandReply(ctx, unavailable("speaker", "State could not be saved"), "persistence_error", null, { newoSpeak: false }); }
      setSpeakerAccepting(true);
    } else {
      setSpeakerAccepting(false);
    }

    const status = await requestSpeakerStatus(ctx, "set_enabled", { enabled });
    if (!enabled) {
      try { await persistSpeakerEnabled(false); }
      catch { return commandReply(ctx, unavailable("speaker", "Speaker is OFF but state could not be saved"), "persistence_error", status.request.requestId ?? null, { newoSpeak: false }); }
    }

    // `/speaker` is intentionally terse and never speaks its own toggle reply.
    // Detailed speaker telemetry remains available to the status/control helpers.
    const text = enabled ? "Speaker turned on." : "Speaker turned off.";
    return commandReply(ctx, text, status.device ? "response" : "device_unavailable", status.request.requestId ?? null, { newoSpeak: false });
  }

  async function speaker(ctx) {
    if (String(ctx.match ?? "").trim()) return commandReply(ctx, message("speaker", ["Usage: /speaker"]), "usage");
    const operation = speakerToggleQueue.then(() => applySpeakerToggle(ctx));
    speakerToggleQueue = operation.catch(() => {});
    return operation;
  }

  async function eco(ctx) {
    if (String(ctx.match ?? "").trim()) return commandReply(ctx, message("eco", ["Usage: /eco"]), "usage");
    const toggle = sendDeviceRequest("eco_toggle", "display_ack", {}, commandTrace(ctx));
    if (toggle.kind !== "sent") return commandReply(ctx, unavailable("eco", "offline"), "offline");
    const toggled = await toggle.promise;
    if (toggled.kind !== "response") return commandReply(ctx, unavailable("eco", toggled.kind === "timeout" ? "No reply" : "offline"), toggled.kind, toggle.requestId);
    const enabled = toggled.message.mode === "eco_on";
    const telemetry = sendDeviceRequest("status_request", "status", {}, commandTrace(ctx));
    if (telemetry.kind === "sent") await telemetry.promise;
    return commandReply(ctx, formatEcoStatus(enabled, getDeviceSnapshot()), "response", toggle.requestId);
  }

  async function volume(ctx) {
    const parsed = parseVolumeArgument(ctx.match);
    if (parsed.kind === "invalid") return commandReply(ctx, message("volume", ["Usage: /volume [0-100]"]), "usage");
    const status = await requestSpeakerStatus(ctx, parsed.kind === "set" ? "set_volume" : null, parsed.kind === "set" ? { volume: parsed.volume } : {});
    if (!status.device) return commandReply(ctx, unavailable("volume", status.result?.kind === "timeout" ? "No reply" : "offline"), status.result?.kind ?? "offline", status.request.requestId ?? null);
    return commandReply(ctx, formatVolumeStatus(status.device), status.device.applied === false ? "device_error" : "response", status.request.requestId);
  }

  async function mute(ctx) {
    if (String(ctx.match ?? "").trim()) return commandReply(ctx, message("mute", ["Usage: /mute"]), "usage");
    const status = await requestSpeakerStatus(ctx, "toggle_mute");
    if (!status.device) return commandReply(ctx, unavailable("mute", status.result?.kind === "timeout" ? "No reply" : "offline"), status.result?.kind ?? "offline", status.request.requestId ?? null);
    return commandReply(ctx, formatMuteStatus(status.device), status.device.applied === false ? "device_error" : "response", status.request.requestId);
  }

  return { voice, voiceStatus, speaker, eco, volume, mute };
}
