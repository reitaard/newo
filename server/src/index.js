import { timingSafeEqual, randomUUID } from "node:crypto";
import { loadEnvFile } from "node:process";

import Fastify from "fastify";
import { Bot, webhookCallback } from "grammy";
import WebSocket, { WebSocketServer } from "ws";
import { z } from "zod";

import { createAssistantRuntime } from "./assistant.js";
import { createAssistantTurnRuntime } from "./assistant-turn.js";
import { createRuntimeStateStore } from "./runtime-state.js";
import { createSpeakerRuntime, startTelegramAndSpeech } from "./tts.js";
import { createTtsBackend } from "./tts-backend.js";
import { createPrimaryModeHandlers } from "./telegram-mode-commands.js";
import { createVoiceRuntime, NullAsrBackend, WorkerAsrBackend } from "./voice.js";

try {
  loadEnvFile(".env");
} catch (error) {
  if (error?.code !== "ENOENT") throw error;
}

const emptyToUndefined = (value) => {
  if (typeof value !== "string") return value;
  const trimmed = value.trim();
  return trimmed.length === 0 ? undefined : trimmed;
};
const stringToBoolean = (value) => typeof value === "string" ? value.trim().toLowerCase() === "true" : value;

const EnvSchema = z.object({
  HOST: z.preprocess(emptyToUndefined, z.string().default("127.0.0.1")),
  PORT: z.preprocess(emptyToUndefined, z.coerce.number().int().min(1).max(65535).default(8788)),
  PUBLIC_BASE_URL: z.preprocess(emptyToUndefined, z.string().url().default("https://newo.reitaard.de")),
  TELEGRAM_BOT_TOKEN: z.preprocess(emptyToUndefined, z.string().optional()),
  TELEGRAM_WEBHOOK_SECRET: z.preprocess(emptyToUndefined, z.string().min(16).optional()),
  TELEGRAM_ALLOWED_USER_IDS: z.preprocess(emptyToUndefined, z.string().optional()),
  TELEGRAM_ALLOWED_CHAT_IDS: z.preprocess(emptyToUndefined, z.string().optional()),
  NEWO_DEVICE_ID: z.preprocess(emptyToUndefined, z.string().min(1).default("newo-01")),
  NEWO_DEVICE_SECRET: z.preprocess(emptyToUndefined, z.string().min(24).optional()),
  VOICE_SAMPLE_RATE: z.preprocess(emptyToUndefined, z.coerce.number().int().positive().default(16_000)),
  VOICE_CHANNELS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(1).max(2).default(1)),
  VOICE_BITS_PER_SAMPLE: z.preprocess(emptyToUndefined, z.coerce.number().int().refine((value) => [8, 16, 24, 32].includes(value)).default(16)),
  VOICE_MAX_STREAM_BYTES: z.preprocess(emptyToUndefined, z.coerce.number().int().positive().default(19_200_000)),
  VOICE_MAX_CHUNK_BYTES: z.preprocess(emptyToUndefined, z.coerce.number().int().positive().max(256 * 1024).default(64 * 1024)),
  VOICE_SAVE_WAV: z.preprocess(stringToBoolean, z.boolean().default(false)),
  VOICE_CAPTURE_DIRECTORY: z.preprocess(emptyToUndefined, z.string().default("/tmp/newo-voice")),
  VOICE_ASR_BACKEND: z.preprocess(emptyToUndefined, z.enum(["null", "sherpa"]).default("sherpa")),
  VOICE_SHERPA_MODEL: z.preprocess(emptyToUndefined, z.enum(["20m", "libri-giga"]).default("libri-giga")),
  VOICE_ASR_MODEL_DIRECTORY: z.preprocess(emptyToUndefined, z.string().optional()),
  VOICE_ASR_THREADS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(1).max(6).default(2)),
  VOICE_ASR_HOTWORDS_ENABLED: z.preprocess(stringToBoolean, z.boolean().default(true)),
  VOICE_ASR_HOTWORDS_FILE: z.preprocess(emptyToUndefined, z.string().default("config/newo-hotwords.txt")),
  VOICE_ASR_HOTWORDS_SCORE: z.preprocess(emptyToUndefined, z.coerce.number().min(0).max(5).default(1.5)),
  VOICE_LIVE_TEST_MODE: z.preprocess(stringToBoolean, z.boolean().default(false)),
  ASSISTANT_ENABLED: z.preprocess(stringToBoolean, z.boolean().default(false)),
  ASSISTANT_BASE_URL: z.preprocess(emptyToUndefined, z.string().url().optional()),
  ASSISTANT_MODEL: z.preprocess(emptyToUndefined, z.string().min(1).optional()),
  ASSISTANT_API_KEY: z.preprocess(emptyToUndefined, z.string().optional()),
  ASSISTANT_TIMEOUT_MS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(1_000).max(30_000).default(15_000)),
  ASSISTANT_MAX_OUTPUT_TOKENS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(8).max(128).default(48)),
  ASSISTANT_MAX_REPLY_CHARS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(40).max(500).default(240)),
  TTS_ENABLED: z.preprocess(stringToBoolean, z.boolean().default(false)),
  SPEAKER_CODEC: z.preprocess(emptyToUndefined, z.enum(["opus", "pcm"]).default("opus")),
  TTS_BACKEND: z.preprocess(emptyToUndefined, z.enum(["pocket", "kokoro", "espeak"]).default("pocket")),
  TTS_VOICE: z.preprocess(emptyToUndefined, z.string().min(1).optional()),
  TTS_SPEED: z.preprocess(emptyToUndefined, z.coerce.number().min(0.25).max(4).default(1)),
  TTS_SAMPLE_RATE: z.preprocess(emptyToUndefined, z.coerce.number().int().refine((value) => value === 24_000, "speaker output must be 24000 Hz").default(24_000)),
  TTS_RATE: z.preprocess(emptyToUndefined, z.coerce.number().int().min(80).max(450).default(155)),
  TTS_GAIN_DB: z.preprocess(emptyToUndefined, z.coerce.number().min(-12).max(18).optional()),
  TTS_MAX_TEXT_CHARS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(20).max(500).default(300)),
  TTS_MAX_PCM_BYTES: z.preprocess(emptyToUndefined, z.coerce.number().int().positive().max(2_880_000).default(2_880_000)),
  KOKORO_BASE_URL: z.preprocess(emptyToUndefined, z.string().url().default("http://127.0.0.1:8010")),
  KOKORO_REQUEST_TIMEOUT_MS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(1_000).max(120_000).default(30_000)),
  KOKORO_STREAM_NO_PROGRESS_MS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(1_000).max(30_000).default(10_000)),
  KOKORO_STREAM_ABSOLUTE_MS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(10_000).max(75_000).default(70_000)),
  POCKET_BASE_URL: z.preprocess(emptyToUndefined, z.string().url().default("http://127.0.0.1:8123")),
  POCKET_REQUEST_TIMEOUT_MS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(1_000).max(120_000).default(30_000)),
  POCKET_STREAM_NO_PROGRESS_MS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(1_000).max(30_000).default(10_000)),
  POCKET_STREAM_ABSOLUTE_MS: z.preprocess(emptyToUndefined, z.coerce.number().int().min(10_000).max(75_000).default(70_000)),
  RUNTIME_STATE_FILE: z.preprocess(emptyToUndefined, z.string().default("data/runtime-state.json")),
});

const env = EnvSchema.parse(process.env);
// OpusPlaybackTransport reads this existing process setting directly.
process.env.SPEAKER_CODEC = env.SPEAKER_CODEC;
if (env.TELEGRAM_BOT_TOKEN && !env.TELEGRAM_WEBHOOK_SECRET) throw new Error("TELEGRAM_WEBHOOK_SECRET is required when TELEGRAM_BOT_TOKEN is set");

const parseIdSet = (value) => new Set((value ?? "").split(",").map((item) => item.trim()).filter(Boolean));
const allowedUserIds = parseIdSet(env.TELEGRAM_ALLOWED_USER_IDS);
const allowedChatIds = parseIdSet(env.TELEGRAM_ALLOWED_CHAT_IDS);

const app = Fastify({ logger: true, trustProxy: true, bodyLimit: 256 * 1024 });
const wss = new WebSocketServer({ noServer: true, perMessageDeflate: false, maxPayload: 64 * 1024 });
const voiceWss = new WebSocketServer({ noServer: true, perMessageDeflate: false, maxPayload: env.VOICE_MAX_CHUNK_BYTES });
const speakerWss = new WebSocketServer({ noServer: true, perMessageDeflate: false, maxPayload: 4 * 1024 });
const sherpaModelDirectories = {
  "20m": "models/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17",
  "libri-giga": "models/sherpa-onnx-streaming-zipformer-en-2023-06-21",
};
const voiceModelDirectory = env.VOICE_ASR_MODEL_DIRECTORY ?? sherpaModelDirectories[env.VOICE_SHERPA_MODEL];
const voiceAsr = env.VOICE_ASR_BACKEND === "sherpa"
  ? new WorkerAsrBackend({
    modelDirectory: voiceModelDirectory,
    model: env.VOICE_SHERPA_MODEL,
    numThreads: env.VOICE_ASR_THREADS,
    hotwordsFile: env.VOICE_SHERPA_MODEL === "libri-giga" && env.VOICE_ASR_HOTWORDS_ENABLED ? env.VOICE_ASR_HOTWORDS_FILE : undefined,
    hotwordsScore: env.VOICE_ASR_HOTWORDS_SCORE,
  })
  : new NullAsrBackend();
const devices = new Map();
const pendingRequests = new Map();
const DEVICE_REQUEST_TIMEOUT_MS = 5_000;
const OFFLINE_GRACE_MS = 12_000;
const REBOOT_RETURN_TIMEOUT_MS = 60_000;
const FACE_STYLES = ["default", "happy", "angry", "tired", "curious", "confused", "laugh", "sweat", "cyclops"];
let bot = null;
let pendingReboot = null;
let shuttingDown = false;
const runtimeState = createRuntimeStateStore({ filePath: env.RUNTIME_STATE_FILE, logger: app.log });
let automaticSpeakerEnabled = runtimeState.speakerEnabled;

const ttsBackend = createTtsBackend(env, app.log);

const speakerRuntime = createSpeakerRuntime({
  logger: app.log,
  enabled: env.TTS_ENABLED,
  backend: ttsBackend,
  getDevice: () => getConnectedDeviceState(),
  isPersistentEnabled: () => automaticSpeakerEnabled,
  sendControl(message, device) {
    if (!device || devices.get(env.NEWO_DEVICE_ID) !== device || device.ws.readyState !== WebSocket.OPEN) return false;
    return new Promise((resolve) => {
      let settled = false;
      const finish = (sent) => { if (settled) return; settled = true; clearTimeout(timer); resolve(sent); };
      const timer = setTimeout(() => finish(false), 1_000);
      timer.unref();
      try { device.ws.send(JSON.stringify(message), (error) => finish(!error)); } catch { finish(false); }
    });
  },
  maxTextChars: env.TTS_MAX_TEXT_CHARS,
  format: { sampleRate: env.TTS_SAMPLE_RATE, channels: 1, bitsPerSample: 16 },
});
const assistantRuntime = createAssistantRuntime({
  enabled: env.ASSISTANT_ENABLED,
  baseUrl: env.ASSISTANT_BASE_URL,
  model: env.ASSISTANT_MODEL,
  apiKey: env.ASSISTANT_API_KEY,
  timeoutMs: env.ASSISTANT_TIMEOUT_MS,
  maxOutputTokens: env.ASSISTANT_MAX_OUTPUT_TOKENS,
  maxReplyChars: env.ASSISTANT_MAX_REPLY_CHARS,
  logger: app.log,
});
const assistantTurnRuntime = createAssistantTurnRuntime({
  assistant: assistantRuntime,
  speakerRuntime,
  isPersistentSpeakerEnabled: () => automaticSpeakerEnabled,
  maxReplyChars: env.ASSISTANT_MAX_REPLY_CHARS,
  logger: app.log,
  setAssistantState(deviceId, state) {
    const device = devices.get(deviceId);
    if (!device || device.ws.readyState !== WebSocket.OPEN) return false;
    try { device.ws.send(JSON.stringify({ type: "assistant_state", state })); return true; }
    catch { return false; }
  },
});
const voiceRuntime = createVoiceRuntime({
  logger: app.log,
  asr: voiceAsr,
  config: {
    sampleRate: env.VOICE_SAMPLE_RATE,
    channels: env.VOICE_CHANNELS,
    bitsPerSample: env.VOICE_BITS_PER_SAMPLE,
    maxStreamBytes: env.VOICE_MAX_STREAM_BYTES,
    saveWav: env.VOICE_SAVE_WAV,
    captureDirectory: env.VOICE_CAPTURE_DIRECTORY,
    liveTestMode: env.VOICE_LIVE_TEST_MODE,
    maxPendingChunks: 2,
    onFinalTranscript: (turn) => assistantTurnRuntime.handleFinalTranscript(turn),
  },
});

const DeviceMessageSchema = z.discriminatedUnion("type", [
  z.object({ type: z.literal("hello"), device: z.string().min(1), firmware: z.string().optional(), chip: z.string().optional() }).passthrough(),
  z.object({ type: z.literal("pong"), request_id: z.string().optional(), uptime_ms: z.number().nonnegative().optional(), rssi: z.number().optional(), ssid: z.string().optional() }).passthrough(),
  z.object({ type: z.literal("status"), request_id: z.string().optional(), uptime_ms: z.number().nonnegative().optional(), rssi: z.number().optional(), ssid: z.string().optional(), free_heap: z.number().nonnegative().optional(), free_psram: z.number().nonnegative().optional() }).passthrough(),
  z.object({ type: z.literal("reboot_ack"), request_id: z.string().optional() }).passthrough(),
  z.object({ type: z.literal("voice_ack"), request_id: z.string(), state: z.enum(["off", "armed", "streaming"]), voice_connected: z.boolean(), wake_count: z.number().nonnegative(), session_count: z.number().nonnegative(), failures: z.number().nonnegative().optional(), timeouts: z.number().nonnegative().optional(), applied: z.boolean().optional() }).passthrough(),
  z.object({
    type: z.literal("health"), request_id: z.string(), firmware: z.string(), uptime_ms: z.number().nonnegative(), reset_reason: z.number().int(), ssid: z.string().optional(), rssi: z.number().optional(), cloud_connected: z.boolean(), free_heap: z.number().nonnegative(), min_free_heap: z.number().nonnegative(), free_psram: z.number().nonnegative(), largest_free_internal_block: z.number().nonnegative().optional(),
    voice: z.object({ state: z.enum(["off", "armed", "streaming"]), connected: z.boolean(), wakes: z.number(), sessions: z.number(), failures: z.number(), timeouts: z.number() }).optional(),
    wifi: z.object({ scans: z.number(), connect_attempts: z.number(), connections: z.number(), failed: z.number(), disconnects: z.number(), last_disconnect_reason: z.string() }),
    cloud: z.object({ connections: z.number(), disconnects: z.number(), errors: z.number() }),
    logs: z.object({ stored: z.number().int(), capacity: z.number().int(), warnings: z.number(), errors: z.number() }),
  }),
  z.object({ type: z.literal("display_ack"), request_id: z.string().optional(), mode: z.string().max(16) }).passthrough(),
  z.object({ type: z.literal("speaker_ack"), request_id: z.string(), enabled: z.boolean(), connection: z.enum(["Ready", "Connecting", "Disconnected"]), volume: z.number().int().min(0).max(100), muted: z.boolean(), applied: z.boolean(), last_playback: z.enum(["None", "Playing", "Complete", "Failed"]), underruns: z.number().int().nonnegative(), overflows: z.number().int().nonnegative(), buffer_bytes: z.number().int().positive() }).passthrough(),
  z.object({ type: z.literal("speaker_started"), playback_id: z.string().uuid(), first_pcm_to_play_ms: z.number().int().nonnegative() }).passthrough(),
  z.object({ type: z.literal("speaker_complete"), playback_id: z.string().uuid(), bytes: z.number().int().nonnegative() }).passthrough(),
  z.object({ type: z.literal("speaker_error"), playback_id: z.string().uuid(), bytes: z.number().int().nonnegative(), error: z.string().max(48) }).passthrough(),
  z.object({ type: z.literal("logs"), request_id: z.string(), firmware: z.string(), uptime_ms: z.number().nonnegative(), warnings: z.number().nonnegative(), errors: z.number().nonnegative(), error: z.string().optional(), entries: z.array(z.object({ seq: z.number(), first_ms: z.number(), last_ms: z.number(), repeat: z.number().int().positive(), level: z.enum(["info", "warn", "error"]), subsystem: z.string(), code: z.string(), detail: z.string().max(96) })).max(40) }),
]);

function safeEqual(left, right) {
  if (typeof left !== "string" || typeof right !== "string") return false;
  const leftBuffer = Buffer.from(left);
  const rightBuffer = Buffer.from(right);
  return leftBuffer.length === rightBuffer.length && timingSafeEqual(leftBuffer, rightBuffer);
}

function rejectUpgrade(socket, statusCode, statusText) {
  socket.write(`HTTP/1.1 ${statusCode} ${statusText}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
  socket.destroy();
}

function getDeviceSnapshot() {
  const device = devices.get(env.NEWO_DEVICE_ID);
  const connected = device?.ws?.readyState === WebSocket.OPEN;
  return { connected, id: env.NEWO_DEVICE_ID, connected_at: device?.connectedAt ?? null, last_seen: device?.lastSeen ?? null, hello: device?.hello ?? null, status: device?.status ?? null };
}

function getConnectedDeviceState() {
  const device = devices.get(env.NEWO_DEVICE_ID);
  return device?.ws?.readyState === WebSocket.OPEN ? device : null;
}

function settlePendingRequest(requestId, result) {
  const pending = pendingRequests.get(requestId);
  if (!pending) {
    app.log.info({ request_id: requestId, settlement: result.kind }, "Ignored already-settled Newo request");
    return false;
  }
  pendingRequests.delete(requestId);
  clearTimeout(pending.timer);
  app.log.info({ request_id: requestId, request_type: pending.requestType, expected_response_type: pending.responseType, telegram_update_id: pending.trace?.updateId ?? null, settlement: result.kind, elapsed_ms: Math.max(0, Date.now() - pending.startedAt) }, "Newo request settled");
  pending.resolve(result);
  return true;
}

function createRequestId() {
  let requestId;
  do requestId = randomUUID(); while (pendingRequests.has(requestId));
  return requestId;
}

function sendDeviceRequest(requestType, responseType, fields = {}, trace = null) {
  const device = getConnectedDeviceState();
  if (!device) return { kind: "offline" };
  const requestId = createRequestId();
  const startedAt = Date.now();
  let timer;
  let resolveRequest;
  const promise = new Promise((resolve) => { resolveRequest = resolve; });
  timer = setTimeout(() => settlePendingRequest(requestId, { kind: "timeout" }), DEVICE_REQUEST_TIMEOUT_MS);
  timer.unref();
  pendingRequests.set(requestId, { deviceId: env.NEWO_DEVICE_ID, ws: device.ws, requestType, responseType, startedAt, timer, resolve: resolveRequest, trace });
  app.log.info({ request_id: requestId, request_type: requestType, expected_response_type: responseType, telegram_update_id: trace?.updateId ?? null, created_at: new Date(startedAt).toISOString() }, "Newo request created");
  try {
    device.ws.send(JSON.stringify({ type: requestType, request_id: requestId, ...fields }), (error) => { if (error) settlePendingRequest(requestId, { kind: "send_error" }); });
  } catch {
    settlePendingRequest(requestId, { kind: "send_error" });
  }
  return { kind: "sent", requestId, promise };
}

function resolvePendingResponse(deviceId, ws, message) {
  if (!message.request_id) return false;
  const pending = pendingRequests.get(message.request_id);
  if (!pending || pending.deviceId !== deviceId || pending.ws !== ws || pending.responseType !== message.type) return false;
  return settlePendingRequest(message.request_id, { kind: "response", message, elapsedMs: Math.max(0, Date.now() - pending.startedAt) });
}

function failPendingRequestsForDevice(deviceId, ws, kind = "disconnected") {
  for (const [requestId, pending] of pendingRequests) if (pending.deviceId === deviceId && pending.ws === ws) settlePendingRequest(requestId, { kind });
}

function formatDuration(milliseconds) {
  if (!Number.isFinite(milliseconds) || milliseconds < 0) return "unknown";
  let seconds = Math.floor(milliseconds / 1_000);
  if (seconds < 1) return `${Math.floor(milliseconds)} ms`;
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

function escapeHtml(value) { return String(value ?? "").replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;"); }
function bold(value) { return `<b>${escapeHtml(value)}</b>`; }
function italic(value) { return `<i>${escapeHtml(value)}</i>`; }
function boldItalic(value) { return `<b><i>${escapeHtml(value)}</i></b>`; }
function title(value) { return boldItalic(value); }
function quote(lines) { return `<blockquote>${lines.join("\n")}</blockquote>`; }
function commandMessage(name, blocks) { return `${title(`${String(name).toLowerCase()}:`)}\n${blocks.join("\n")}`; }
function statusMessage(name, status) { return commandMessage(name, [quote([`Status: ${bold(status)}`])]); }
function timeoutMessage(name) { return commandMessage(name, [quote([`Status: ${bold("No reply")}`, `Timeout: ${bold(5)} ${italic("seconds")}`])]); }

function formatDeviceStatus(snapshot, source = "live") {
  if (!snapshot.connected) return statusMessage("status", "offline");
  const status = snapshot.status ?? {};
  const hello = snapshot.hello ?? {};
  return commandMessage("status", [quote([
    `Status: ${bold("online")}${source === "cached" ? ` ${italic("cached")}` : ""}`,
    `Wi-Fi: ${bold(status.ssid ?? "unknown")}`,
    `Signal: ${bold(typeof status.rssi === "number" ? status.rssi : "unknown")}${typeof status.rssi === "number" ? ` ${italic("dBm")}` : ""}`,
    `Uptime: ${boldItalic(formatDuration(status.uptime_ms))}`,
    `Firmware: ${bold(hello.firmware ?? "unknown")}`,
  ])]);
}

function cancelOfflineTimer(state) {
  if (state?.offlineTimer) { clearTimeout(state.offlineTimer); state.offlineTimer = null; }
}

function sendConnectivityNotification(html) {
  if (!bot || allowedChatIds.size === 0) return;
  for (const chatId of allowedChatIds) {
    void Promise.resolve().then(() => bot.api.sendMessage(chatId, html, { parse_mode: "HTML" })).catch(() => app.log.warn({ chat_id: chatId }, "Failed to send Newo connectivity notification"));
  }
}

function scheduleOfflineNotification(deviceId, state, ws) {
  if (shuttingDown || !state.hasBeenConnected || pendingReboot?.deviceId === deviceId) return;
  cancelOfflineTimer(state);
  state.offlineSince = Date.now();
  state.offlineNotified = false;
  state.offlineTimer = setTimeout(() => {
    state.offlineTimer = null;
    const current = devices.get(deviceId);
    if (shuttingDown || pendingReboot?.deviceId === deviceId || current !== state || current.ws !== ws || current.ws.readyState === WebSocket.OPEN) return;
    state.offlineNotified = true;
    sendConnectivityNotification(statusMessage("connectivity", "offline"));
  }, OFFLINE_GRACE_MS);
  state.offlineTimer.unref();
}

app.get("/", async () => ({ service: "newo-cloud", status: "ok" }));
app.get("/health", async () => {
  const device = getDeviceSnapshot();
  return { status: "ok", service: "newo-cloud", uptime_s: Math.floor(process.uptime()), telegram_enabled: Boolean(env.TELEGRAM_BOT_TOKEN), device: { connected: device.connected, id: device.id, connected_at: device.connected_at, last_seen: device.last_seen, firmware: device.hello?.firmware ?? null, chip: device.hello?.chip ?? null } };
});

const TELEGRAM_COMMANDS = [];
function commandTrace(ctx) { return ctx.newoTrace ?? null; }
async function commandReply(ctx, text, category = "reply", requestId = null, options = {}) {
  const trace = commandTrace(ctx);
  if (trace) { trace.replyCategory = category; trace.requestId = requestId; }
  const { newoSpeak = true, newoSpeakMaxChars, ...telegramOptions } = options;
  const replyReadyAt = performance.now();
  // Start Telegram first, then immediately start independent TTS without waiting
  // for Telegram's network request. Neither failure path is allowed to poison the other.
  return startTelegramAndSpeech(
    () => ctx.reply(text, { parse_mode: "HTML", ...telegramOptions }),
    () => {
      if (!newoSpeak || !automaticSpeakerEnabled || (trace && trace.speechQueued)) return;
      if (trace) trace.speechQueued = true;
      const speech = speakerRuntime.speak(text, { maxChars: newoSpeakMaxChars, replyReadyAt });
      if (speech.kind === "queued") void speech.completion.catch((error) => app.log.warn({ playback_id: speech.playbackId, error_message: error.message }, "Asynchronous Telegram speech failed"));
    },
  );
}

function showDisplay(text) {
  const request = sendDeviceRequest("display_set", "display_ack", { mode: "message", text: String(text).slice(0, 96), temporary: true });
  if (request.kind === "sent") void request.promise;
}

async function handleNewoCommand(ctx) {
  const input = String(ctx.match ?? "").trim();
  if (input) return commandReply(ctx, commandMessage("newo", [quote(["Reserved for the Newo agent. Use /face for display styles."])]), "reserved");
  return commandReply(ctx, commandMessage("newo", [quote(["Reserved for the Newo agent."])]), "reserved");
}

function faceStyleCommand(style) { return `/face_${style}`; }

async function handleFaceCommand(ctx, selectedInput = null) {
  const input = selectedInput ?? String(ctx.match ?? "").trim().toLowerCase();
  if (!input) {
    return commandReply(ctx, commandMessage("face", [quote(FACE_STYLES.map(faceStyleCommand))]), "usage");
  }
  if (!FACE_STYLES.includes(input)) {
    return commandReply(ctx, commandMessage("face", [quote(["Choose one:", ...FACE_STYLES.map(faceStyleCommand)])]), "usage");
  }
  const request = sendDeviceRequest("display_set", "display_ack", { mode: input, text: "" }, commandTrace(ctx));
  if (request.kind === "offline") return commandReply(ctx, statusMessage("face", "offline"), "offline");
  const result = await request.promise;
  if (result.kind === "response") return commandReply(ctx, commandMessage("face", [quote([`Face: ${bold(input)}`])]), "response", request.requestId);
  return commandReply(ctx, commandMessage("face", [quote(["Face update was not acknowledged."])]), result.kind, request.requestId);
}

async function handleStatusCommand(ctx) {
  const request = sendDeviceRequest("status_request", "status", {}, commandTrace(ctx));
  if (request.kind === "offline") { await commandReply(ctx, statusMessage("status", "offline"), "offline"); return; }
  const result = await request.promise;
  if (result.kind === "response") {
    const state = getDeviceSnapshot();
    showDisplay(`STATUS\nOnline ${state.connected ? "YES" : "NO"}\nWiFi ${state.status?.rssi ?? "?"}\nHeap ${Math.round((state.status?.free_heap ?? 0) / 1024)}K\nPSRAM ${Math.round((state.status?.free_psram ?? 0) / 1024)}K`);
    await commandReply(ctx, formatDeviceStatus(state), "response", request.requestId);
    return;
  }
  if (result.kind === "timeout") {
    const snapshot = getDeviceSnapshot();
    if (snapshot.status) await commandReply(ctx, formatDeviceStatus(snapshot, "cached"), "cached", request.requestId);
    else await commandReply(ctx, timeoutMessage("status"), "timeout", request.requestId);
    return;
  }
  await commandReply(ctx, getConnectedDeviceState() ? timeoutMessage("status") : statusMessage("status", "offline"), "unavailable", request.requestId);
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes < 0) return "unknown";
  if (bytes >= 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
  return `${Math.round(bytes / 1024)} KB`;
}
function formatResetReason(reason) { return ({ 1: "Power on", 2: "External reset", 3: "Software restart", 4: "Crash", 5: "Watchdog", 6: "Watchdog", 7: "Watchdog", 8: "Deep sleep wake", 9: "Brownout" })[reason] ?? "Unknown"; }
function parseLogsArguments(match, forceProblems = false) {
  const parts = String(match ?? "").trim().toLowerCase().split(/\s+/).filter(Boolean);
  let limit = 20;
  let minLevel = forceProblems ? "warn" : "info";
  for (const part of parts) {
    if (/^\d+$/.test(part) && limit === 20) limit = Number(part);
    else if (["warn", "error"].includes(part) && minLevel === (forceProblems ? "warn" : "info")) minLevel = part;
    else return null;
  }
  if (!Number.isInteger(limit) || limit < 1 || limit > 40) return null;
  return { limit, minLevel };
}
function formatLogTime(milliseconds) {
  const seconds = Math.max(0, Math.floor(milliseconds / 1000));
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const remainder = seconds % 60;
  return hours ? `${hours}:${String(minutes).padStart(2, "0")}:${String(remainder).padStart(2, "0")}` : `${String(minutes).padStart(2, "0")}:${String(remainder).padStart(2, "0")}`;
}
function parseLogDetail(code, detail) {
  const text = String(detail ?? "");
  const valueAfter = (prefix) => text.startsWith(prefix) ? text.slice(prefix.length) : undefined;
  switch (code) {
    case "WIFI_CONNECTING": return { ssid: valueAfter("ssid=") };
    case "WIFI_CONNECTED": {
      const marker = " rssi="; const index = text.lastIndexOf(marker);
      if (index === -1 || !text.startsWith("ssid=")) return {};
      return { ssid: text.slice("ssid=".length, index), rssi: text.slice(index + marker.length) };
    }
    case "WIFI_CONNECT_FAILED": {
      const marker = " reason="; const index = text.lastIndexOf(marker);
      if (index === -1 || !text.startsWith("ssid=")) return {};
      return { ssid: text.slice("ssid=".length, index), reason: text.slice(index + marker.length) };
    }
    case "WIFI_DISCONNECTED": return { reason: valueAfter("reason=") };
    default: return Object.fromEntries(text.split(" ").map((part) => part.split(/=(.*)/, 2)).filter(([key, value]) => key && value !== undefined));
  }
}
function formatLogEntry(entry) {
  const detail = parseLogDetail(entry.code, entry.detail);
  const withTimestamp = (event, details = []) => `${italic(formatLogTime(entry.last_ms))}  ${bold(event)}${details.length ? ` — ${details.join(", ")}` : ""}${entry.repeat > 1 ? ` ${italic(`×${entry.repeat}`)}` : ""}`;
  if (entry.code === "WIFI_CONNECTING") return withTimestamp("Connecting", [escapeHtml(detail.ssid ?? "Wi-Fi")]);
  if (entry.code === "WIFI_CONNECTED") {
    const details = [escapeHtml(detail.ssid ?? "unknown")]; if (detail.rssi) details.push(`${escapeHtml(detail.rssi)} ${italic("dBm")}`); return withTimestamp("Wi-Fi connected", details);
  }
  if (entry.code === "WIFI_CONNECT_FAILED") return withTimestamp("Wi-Fi connection failed", [escapeHtml(detail.ssid ?? "unknown"), escapeHtml(detail.reason ?? "unknown reason")]);
  const labels = {
    BOOT_START: ["Started"], BOOT_READY: ["Newo ready"], STORAGE_READY: ["Storage ready", `${detail.saved_networks ?? "0"} networks`], STORAGE_FAILED: ["Storage failed"],
    WIFI_SCAN_START: ["Scanning for saved Wi-Fi"], WIFI_SCAN_EMPTY: ["Wi-Fi scan found nothing"], WIFI_SCAN_RESULT: ["Found saved Wi-Fi"], WIFI_DISCONNECTED: ["Wi-Fi disconnected", detail.reason ?? "unknown reason"], WIFI_RECONNECTING: ["Reconnecting Wi-Fi"],
    PROV_STARTED: ["Wi-Fi setup started"], PROV_READY: ["Wi-Fi setup ready"], PROV_CREDENTIALS_RECEIVED: ["Wi-Fi details received"], PROV_CREDENTIALS_ACCEPTED: ["Wi-Fi details accepted"], PROV_CREDENTIALS_REJECTED: ["Wi-Fi details rejected"], PROV_SAVE_FAILED: ["Wi-Fi setup could not save"], PROV_SAVED: ["Wi-Fi network saved"], PROV_STOPPED: ["Wi-Fi setup stopped"], PROV_TIMEOUT: ["Wi-Fi setup timed out"],
    CLOUD_CONFIGURED: ["Cloud configured"], CLOUD_DISABLED: ["Cloud disabled"], CLOUD_CONNECTING: ["Connecting to cloud"], CLOUD_CONNECTED: ["Cloud connected"], CLOUD_AUTHENTICATED: ["Newo authenticated"], CLOUD_DISCONNECTED: ["Cloud disconnected"], CLOUD_WS_ERROR: ["Cloud connection error"], CLOUD_INVALID_JSON: ["Cloud sent invalid data"], CLOUD_UNSUPPORTED_MESSAGE: ["Cloud sent unsupported message"], CLOUD_REBOOT_ACK_FAILED: ["Restart acknowledgement failed"], SYSTEM_REBOOT_REQUESTED: ["Restart requested"], SYSTEM_REBOOTING: ["Restarting"],
  };
  const [event, ...details] = labels[entry.code] ?? [entry.code.replaceAll("_", " ")];
  return withTimestamp(event, details.filter(Boolean).map(escapeHtml));
}
async function replyLogLines(ctx, name, lines, requestId = null, prefixBlocks = []) {
  const header = title(`${String(name).toLowerCase()}:`);
  let chunk = [];
  for (const line of lines) {
    const candidate = `${header}\n${[...prefixBlocks, quote([...chunk, line])].join("\n")}`;
    if (chunk.length && candidate.length > 3800) { await commandReply(ctx, `${header}\n${[...prefixBlocks, quote(chunk)].join("\n")}`, "logs_chunk", requestId); chunk = []; }
    chunk.push(line);
  }
  if (chunk.length || prefixBlocks.length) await commandReply(ctx, `${header}\n${[...prefixBlocks, quote(chunk)].join("\n")}`, "logs_chunk", requestId);
}
async function handleHealthCommand(ctx) {
  const request = sendDeviceRequest("health_request", "health", {}, commandTrace(ctx));
  if (request.kind === "offline") return commandReply(ctx, statusMessage("health", "offline"), "offline");
  const result = await request.promise;
  if (result.kind !== "response") return commandReply(ctx, result.kind === "timeout" ? timeoutMessage("health") : statusMessage("health", "offline"), result.kind, request.requestId);
  const health = result.message;
  showDisplay(`HEALTH\nWiFi ${health.wifi.connections ? "OK" : "OFF"}\nCloud ${health.cloud_connected ? "OK" : "OFF"}\nHeap ${Math.round(health.free_heap / 1024)}K\nPSRAM ${Math.round(health.free_psram / 1024)}K\nWarn ${health.logs.warnings}\nErrors ${health.logs.errors}`);
  const value = (label, data) => `${label}: ${bold(data)}`;
  await commandReply(ctx, commandMessage("health", [
    quote(["System", value("Firmware", health.firmware), `Uptime: ${boldItalic(formatDuration(health.uptime_ms))}`, value("Last restart", formatResetReason(health.reset_reason))]),
    quote(["Network", value("Wi-Fi", health.ssid || "not connected"), `Signal: ${bold(typeof health.rssi === "number" ? health.rssi : "unknown")}${typeof health.rssi === "number" ? ` ${italic("dBm")}` : ""}`, value("Cloud", health.cloud_connected ? "connected" : "not connected")]),
    quote(["Memory", `Heap: ${bold(formatBytes(health.free_heap))} ${italic("free")}`, value("Lowest heap", formatBytes(health.min_free_heap)), value("Largest internal block", formatBytes(health.largest_free_internal_block ?? 0)), `PSRAM: ${bold(formatBytes(health.free_psram))} ${italic("free")}`]),
    quote(["Voice", value("State", (health.voice?.state ?? "off").toUpperCase()), value("Wake detections", health.voice?.wakes ?? 0), value("Sessions", health.voice?.sessions ?? 0), value("Failures / timeouts", `${health.voice?.failures ?? 0} / ${health.voice?.timeouts ?? 0}`)]),
    quote(["Wi-Fi activity", value("Scans", health.wifi.scans), value("Attempts", health.wifi.connect_attempts), value("Connections", health.wifi.connections), value("Failed", health.wifi.failed), value("Disconnects", health.wifi.disconnects), value("Last disconnect", health.wifi.last_disconnect_reason || "Unknown")]),
    quote(["Cloud activity", value("Connections", health.cloud.connections), value("Disconnects", health.cloud.disconnects), value("Errors", health.cloud.errors)]),
    quote(["Logs", value("Stored", `${health.logs.stored} / ${health.logs.capacity}`), value("Warnings", health.logs.warnings), value("Errors", health.logs.errors)]),
  ]), "response", request.requestId);
}
async function handleLogsCommand(ctx, forceProblems = false) {
  const name = forceProblems ? "errors" : "logs";
  const args = parseLogsArguments(ctx.match, forceProblems);
  if (!args) return commandReply(ctx, commandMessage(name, [quote(["Usage: /logs [1-40] [warn|error]"])]), "usage");
  const request = sendDeviceRequest("logs_request", "logs", args, commandTrace(ctx));
  if (request.kind === "offline") return commandReply(ctx, statusMessage(name, "offline"), "offline");
  const result = await request.promise;
  if (result.kind !== "response") return commandReply(ctx, result.kind === "timeout" ? timeoutMessage(name) : statusMessage(name, "offline"), result.kind, request.requestId);
  if (result.message.error) return commandReply(ctx, commandMessage(name, [quote([`Status: ${bold("Unavailable")}`])]), "device_error", request.requestId);
  const entries = result.message.entries;
  if (entries.length === 0) return commandReply(ctx, commandMessage(name, [quote([forceProblems ? "No problems since startup." : "No logs since startup."])]), "clean", request.requestId);
  showDisplay(`${forceProblems ? "ERRORS" : "LOGS"}\nStored ${result.message.entries.length}\nWarn ${result.message.warnings}\nErrors ${result.message.errors}`);
  const summary = forceProblems ? [quote(["Summary", `Warnings: ${bold(result.message.warnings)}`, `Errors: ${bold(result.message.errors)}`])] : [];
  await replyLogLines(ctx, name, entries.map(formatLogEntry), request.requestId, summary);
}
async function handlePingCommand(ctx) {
  const request = sendDeviceRequest("ping", "pong", {}, commandTrace(ctx));
  if (request.kind === "offline") return commandReply(ctx, statusMessage("ping", "offline"), "offline");
  const result = await request.promise;
  if (result.kind === "response") { showDisplay(`PING\n${result.elapsedMs} ms`); return commandReply(ctx, commandMessage("ping", [quote([`Latency: ${bold(result.elapsedMs)} ${italic("ms")}`])]), "response", request.requestId); }
  if (result.kind === "timeout") return commandReply(ctx, timeoutMessage("ping"), "timeout", request.requestId);
  return commandReply(ctx, getConnectedDeviceState() ? timeoutMessage("ping") : statusMessage("ping", "offline"), "unavailable", request.requestId);
}
function trackPendingReboot(chatId, messageId, deviceId) {
  if (pendingReboot?.timer) clearTimeout(pendingReboot.timer);
  const tracker = { chatId, messageId, deviceId, timer: null };
  tracker.timer = setTimeout(() => {
    if (pendingReboot !== tracker) return;
    pendingReboot = null;
    void bot?.api.editMessageText(chatId, messageId, statusMessage("reboot", "Not back online yet"), { parse_mode: "HTML" }).catch(() => app.log.warn({ chat_id: chatId }, "Failed to update timed-out reboot message"));
  }, REBOOT_RETURN_TIMEOUT_MS);
  tracker.timer.unref();
  pendingReboot = tracker;
}
function completePendingReboot(deviceId) {
  const tracker = pendingReboot;
  if (!tracker || tracker.deviceId !== deviceId) return false;
  pendingReboot = null;
  clearTimeout(tracker.timer);
  void Promise.resolve().then(() => bot?.api.deleteMessage(tracker.chatId, tracker.messageId)).catch(() => app.log.warn({ chat_id: tracker.chatId }, "Failed to delete Telegram reboot message")).then(() => bot?.api.sendMessage(tracker.chatId, statusMessage("reboot", "Back online"), { parse_mode: "HTML" })).catch(() => app.log.warn({ chat_id: tracker.chatId }, "Failed to send Telegram reboot completion message"));
  return true;
}
async function handleSpeakCommand(ctx) {
  const text = String(ctx.match ?? "").trim();
  if (!text || text.length > 150) return commandReply(ctx, commandMessage("speak", [quote(["Usage: /speak &lt;1-150 characters&gt;"])]), "usage", null, { newoSpeak: false });
  const speech = speakerRuntime.speak(text, { maxChars: 150, temporary: !automaticSpeakerEnabled });
  if (speech.kind !== "queued") return commandReply(ctx, statusMessage("speak", speech.kind === "offline" ? "offline" : "unavailable"), speech.kind, null, { newoSpeak: false });
  await commandReply(ctx, commandMessage("speak", [quote(["Playback queued."])]), "queued", null, { newoSpeak: false });
  void speech.completion.then(
    () => ctx.reply(commandMessage("speak", [quote(["Playback complete."])]), { parse_mode: "HTML" }),
    (error) => ctx.reply(commandMessage("speak", [quote(["Playback failed."])]), { parse_mode: "HTML" }).catch(() => app.log.warn({ playback_id: speech.playbackId, error_message: error.message }, "Failed to report speaker test failure")),
  ).catch(() => {});
}

async function handleRebootCommand(ctx) {
  const request = sendDeviceRequest("reboot", "reboot_ack", {}, commandTrace(ctx));
  if (request.kind === "offline") return commandReply(ctx, statusMessage("reboot", "offline"), "offline");
  const result = await request.promise;
  if (result.kind === "response") {
    showDisplay("REBOOTING");
    const message = await commandReply(ctx, statusMessage("reboot", "Restarting"), "response", request.requestId);
    trackPendingReboot(ctx.chat.id, message.message_id, env.NEWO_DEVICE_ID);
    return;
  }
  if (result.kind === "timeout") return commandReply(ctx, statusMessage("reboot", "Restart was not acknowledged"), "timeout", request.requestId);
  return commandReply(ctx, getConnectedDeviceState() ? statusMessage("reboot", "Restart was not acknowledged") : statusMessage("reboot", "offline"), "unavailable", request.requestId);
}

const primaryModeHandlers = createPrimaryModeHandlers({
  sendDeviceRequest,
  commandReply,
  commandTrace,
  getDeviceSnapshot,
  getSpeakerEnabled: () => automaticSpeakerEnabled,
  setSpeakerAccepting: (enabled) => {
    automaticSpeakerEnabled = enabled;
    speakerRuntime.setPersistentEnabled(enabled);
  },
  persistSpeakerEnabled: (enabled) => runtimeState.setSpeakerEnabled(enabled),
  speakerInfo: {
    ttsEnabled: env.TTS_ENABLED,
    backend: env.TTS_BACKEND,
    format: `${speakerRuntime.format.sampleRate / 1_000} kHz PCM${speakerRuntime.format.bitsPerSample}`,
    bufferBytes: 24_576,
  },
  getAssistantInfo: () => ({ ...assistantTurnRuntime.getTelemetry(), speakerEnabled: automaticSpeakerEnabled }),
});

if (env.TELEGRAM_BOT_TOKEN) {
  bot = new Bot(env.TELEGRAM_BOT_TOKEN);
  bot.use(async (ctx, next) => {
    const text = ctx.message?.text;
    const command = typeof text === "string" ? text.match(/^\/([a-z0-9_]+)/i)?.[1]?.toLowerCase() ?? null : null;
    ctx.newoTrace = { updateId: ctx.update.update_id ?? null, messageId: ctx.message?.message_id ?? null, command, invokedAt: new Date().toISOString(), replyCount: 0 };
    app.log.info({ telegram_update_id: ctx.newoTrace.updateId, telegram_message_id: ctx.newoTrace.messageId, chat_id: ctx.chat?.id?.toString() ?? null, command, handler_invoked_at: ctx.newoTrace.invokedAt }, "Newo Telegram update received");
    const originalReply = ctx.reply.bind(ctx);
    ctx.reply = async (...args) => {
      const trace = ctx.newoTrace; trace.replyCount += 1;
      app.log.info({ telegram_update_id: trace.updateId, request_id: trace.requestId ?? null, result: trace.replyCategory ?? "reply", reply_count: trace.replyCount, reply_already_emitted: trace.replyCount > 1 }, "Newo Telegram reply emitted");
      return originalReply(...args);
    };
    const userId = ctx.from?.id?.toString();
    const chatId = ctx.chat?.id?.toString();
    const allowed = (userId && allowedUserIds.has(userId)) || (chatId && allowedChatIds.has(chatId));
    if (!allowed) { app.log.warn({ user_id: userId ?? null, chat_id: chatId ?? null }, "Rejected Telegram update outside allowlist"); return; }
    await next();
  });
  bot.catch((error) => {
    const ctx = error.ctx; const trace = commandTrace(ctx);
    app.log.error({ telegram_update_id: ctx?.update?.update_id ?? null, telegram_message_id: ctx?.message?.message_id ?? null, command: trace?.command ?? null, error_type: error.error?.name ?? "Error", error_message: error.error?.message ?? "unknown" }, "Newo Telegram update failed");
  });
  bot.command("start", async (ctx) => {
    const state = getDeviceSnapshot().connected ? "online" : "offline";
    await commandReply(ctx, `${title("status:")}\n${quote([`Status: ${bold(state)}`])}`, "response");
  });
  bot.command(["status", "s"], handleStatusCommand);
  bot.command(["health", "h"], handleHealthCommand);
  bot.command(["logs", "l"], (ctx) => handleLogsCommand(ctx));
  bot.command("errors", (ctx) => handleLogsCommand(ctx, true));
  bot.command("e", (ctx) => handleLogsCommand(ctx, true));
  bot.command(["ping", "p"], handlePingCommand);
  bot.command(["reboot", "r"], handleRebootCommand);
  bot.command(["newo", "n"], handleNewoCommand);
  bot.command(["face", "f"], handleFaceCommand);
  for (const style of FACE_STYLES) bot.command(`face_${style}`, (ctx) => handleFaceCommand(ctx, style));
  bot.command("eco", primaryModeHandlers.eco);
  bot.command(["voice", "v"], primaryModeHandlers.voice);
  bot.command("vs", primaryModeHandlers.voiceStatus);
  bot.command("speaker", primaryModeHandlers.speaker);
  bot.command("volume", primaryModeHandlers.volume);
  bot.command("mute", primaryModeHandlers.mute);
  // Hidden physical bring-up command; deliberately omitted from setMyCommands.
  bot.command(["speak", "sp"], handleSpeakCommand);
  void bot.api.setMyCommands(TELEGRAM_COMMANDS).catch(() => app.log.warn("Failed to register the Telegram command menu"));
  app.post("/telegram/webhook", webhookCallback(bot, "fastify", { secretToken: env.TELEGRAM_WEBHOOK_SECRET, onTimeout: "return", timeoutMilliseconds: 9_000 }));
}

app.server.on("upgrade", (request, socket, head) => {
  let pathname;
  try { pathname = new URL(request.url ?? "/", "http://localhost").pathname; } catch { rejectUpgrade(socket, 400, "Bad Request"); return; }
  if (pathname !== "/device" && pathname !== "/voice" && pathname !== "/speaker") { rejectUpgrade(socket, 404, "Not Found"); return; }
  if (!env.NEWO_DEVICE_SECRET) { app.log.error("Rejected Newo WebSocket because NEWO_DEVICE_SECRET is not configured"); rejectUpgrade(socket, 503, "Service Unavailable"); return; }
  const deviceId = request.headers["x-newo-device-id"];
  const authorization = request.headers.authorization;
  const presentedSecret = typeof authorization === "string" && authorization.startsWith("Bearer ") ? authorization.slice("Bearer ".length) : undefined;
  if (!safeEqual(deviceId, env.NEWO_DEVICE_ID) || !safeEqual(presentedSecret, env.NEWO_DEVICE_SECRET)) { app.log.warn({ device_id: deviceId ?? null, path: pathname }, "Rejected unauthenticated Newo WebSocket"); rejectUpgrade(socket, 401, "Unauthorized"); return; }
  const server = pathname === "/voice" ? voiceWss : pathname === "/speaker" ? speakerWss : wss;
  server.handleUpgrade(request, socket, head, (ws) => server.emit("connection", ws, request, deviceId));
});

voiceWss.on("connection", (ws, request, deviceId) => {
  void voiceRuntime.handleConnection(ws, deviceId).catch((error) => { app.log.error({ device_id: deviceId, error_message: error?.message ?? "unknown" }, "Voice connection setup failed"); ws.close(1011, "voice setup failed"); });
});

speakerWss.on("connection", (ws, request, deviceId) => {
  try { speakerRuntime.handleConnection(ws, deviceId); }
  catch (error) { app.log.warn({ device_id: deviceId, error_message: error?.message ?? "unknown" }, "Speaker connection setup failed"); ws.close(1011, "speaker setup failed"); }
});

wss.on("connection", (ws, request, deviceId) => {
  const previous = devices.get(deviceId);
  const reconnectingAfterNotifiedOffline = Boolean(previous?.hasBeenConnected && previous.offlineNotified && previous.offlineSince !== null);
  const offlineDuration = reconnectingAfterNotifiedOffline ? Math.max(0, Date.now() - previous.offlineSince) : 0;
  cancelOfflineTimer(previous);
  if (previous?.ws.readyState === WebSocket.OPEN) { failPendingRequestsForDevice(deviceId, previous.ws, "disconnected"); previous.ws.close(4001, "replaced by new connection"); }
  const state = { ws, connectedAt: new Date().toISOString(), lastSeen: new Date().toISOString(), hello: previous?.hello ?? null, status: previous?.status ?? null, hasBeenConnected: previous?.hasBeenConnected ?? true, offlineSince: null, offlineNotified: false, offlineTimer: null, isAlive: true };
  devices.set(deviceId, state);
  const completedIntentionalReboot = completePendingReboot(deviceId);
  if (reconnectingAfterNotifiedOffline && !completedIntentionalReboot) sendConnectivityNotification(commandMessage("connectivity", [quote([`Status: ${bold("Back online")}`, `Offline for: ${boldItalic(formatDuration(offlineDuration))}`]) ]));
  app.log.info({ device_id: deviceId }, "Newo device connected");
  ws.on("pong", () => { state.isAlive = true; state.lastSeen = new Date().toISOString(); });
  ws.on("message", (raw, isBinary) => {
    const current = devices.get(deviceId);
    if (current !== state || current.ws !== ws) return;
    state.lastSeen = new Date().toISOString();
    if (isBinary) { app.log.warn({ device_id: deviceId }, "Ignoring unexpected binary device message"); return; }
    let json;
    try { json = JSON.parse(raw.toString("utf8")); } catch { app.log.warn({ device_id: deviceId }, "Ignoring invalid JSON from device"); return; }
    const parsed = DeviceMessageSchema.safeParse(json);
    if (!parsed.success) { app.log.warn({ device_id: deviceId, issues: parsed.error.issues }, "Ignoring invalid device message"); return; }
    const message = parsed.data;
    if (message.type === "hello" && message.device !== deviceId) { app.log.warn({ authenticated_device: deviceId, claimed_device: message.device }, "Device hello identity mismatch"); ws.close(4003, "device identity mismatch"); return; }
    if (message.type === "hello") state.hello = { device: message.device, firmware: message.firmware ?? null, chip: message.chip ?? null, received_at: state.lastSeen };
    if (message.type === "status" || message.type === "pong") state.status = { ...(state.status ?? {}), ...message, received_at: state.lastSeen };
    resolvePendingResponse(deviceId, ws, message);
    if (message.type === "speaker_started") speakerRuntime.handlePlaybackStarted(deviceId, message);
    if (message.type === "speaker_complete" || message.type === "speaker_error") speakerRuntime.handleResult(deviceId, message);
    app.log.info({ device_id: deviceId, type: message.type }, "Device message received");
  });
  ws.on("close", (code, reason) => {
    const current = devices.get(deviceId);
    failPendingRequestsForDevice(deviceId, ws, "disconnected");
    assistantTurnRuntime.abortDevice(deviceId);
    if (current?.ws === ws) { current.lastSeen = new Date().toISOString(); scheduleOfflineNotification(deviceId, current, ws); }
    app.log.info({ device_id: deviceId, code, reason: reason.toString() }, "Newo device disconnected");
  });
  ws.on("error", () => app.log.warn({ device_id: deviceId }, "Newo WebSocket error"));
  ws.send(JSON.stringify({ type: "hello_ack", device: deviceId, server_time: new Date().toISOString() }));
  // A fresh device session must never inherit a stale thinking indicator.
  ws.send(JSON.stringify({ type: "assistant_state", state: "idle" }));
  void speakerRuntime.handleDeviceConnected(deviceId, state);
  const speakerSync = sendDeviceRequest("speaker_control", "speaker_ack", { action: "set_enabled", enabled: automaticSpeakerEnabled });
  if (speakerSync.kind === "sent") void speakerSync.promise.then((result) => app.log.info({ device_id: deviceId, enabled: automaticSpeakerEnabled, result: result.kind }, "Speaker mode synchronized"));
});

const heartbeatTimer = setInterval(() => {
  for (const [deviceId, state] of devices) {
    if (state.ws.readyState !== WebSocket.OPEN) continue;
    if (!state.isAlive) { app.log.warn({ device_id: deviceId }, "Terminating stale Newo WebSocket"); state.ws.terminate(); continue; }
    state.isAlive = false;
    state.ws.ping();
  }
}, 30_000);
heartbeatTimer.unref();

async function closeDeviceSocket(ws) {
  if (ws.readyState === WebSocket.CLOSED) return;
  await new Promise((resolve) => {
    let finished = false;
    const finish = () => { if (finished) return; finished = true; clearTimeout(timeout); resolve(); };
    const timeout = setTimeout(() => { ws.terminate(); finish(); }, 1_000); timeout.unref();
    ws.once("close", finish); ws.once("error", finish);
    try { ws.close(1001, "server shutting down"); } catch { ws.terminate(); finish(); }
  });
}

async function shutdown(signal) {
  if (shuttingDown) return;
  shuttingDown = true;
  app.log.info({ signal }, "Shutting down Newo cloud");
  clearInterval(heartbeatTimer);
  if (pendingReboot?.timer) { clearTimeout(pendingReboot.timer); pendingReboot = null; }
  for (const [requestId] of pendingRequests) settlePendingRequest(requestId, { kind: "shutdown" });
  for (const state of devices.values()) cancelOfflineTimer(state);
  assistantTurnRuntime.close();
  speakerRuntime.close();
  await Promise.all([...[...devices.values()].map((state) => state.ws), ...voiceWss.clients, ...speakerWss.clients].filter((ws) => ws.readyState !== WebSocket.CLOSED).map(closeDeviceSocket));
  await Promise.all([wss, voiceWss, speakerWss].map((server) => new Promise((resolve) => { try { server.close(() => resolve()); } catch { resolve(); } })));
  await voiceAsr.close?.();
  await app.close();
  process.exitCode = 0;
}
function handleShutdownSignal(signal) { void shutdown(signal).catch(() => { process.exitCode = 1; }); }
process.once("SIGINT", () => handleShutdownSignal("SIGINT"));
process.once("SIGTERM", () => handleShutdownSignal("SIGTERM"));

await app.listen({ host: env.HOST, port: env.PORT });
// Probe the local model once without generating text. /vs only reads this
// bounded snapshot and never waits on Qwen itself.
void assistantRuntime.refreshHealth();
app.log.info({
  bind: `${env.HOST}:${env.PORT}`,
  public_base_url: env.PUBLIC_BASE_URL,
  websocket_path: "/device",
  voice_websocket_path: "/voice",
  speaker_websocket_path: "/speaker",
  speaker_format: `${speakerRuntime.format.channels}ch ${speakerRuntime.format.sampleRate}Hz ${speakerRuntime.format.bitsPerSample}-bit PCM LE`,
  tts_enabled: env.TTS_ENABLED,
  tts_backend: env.TTS_BACKEND,
  tts_voice: ttsBackend.voice ?? env.TTS_VOICE ?? null,
  tts_speed: env.TTS_SPEED,
  tts_gain_db: ttsBackend.gainDb ?? null,
  voice_format: `${env.VOICE_CHANNELS}ch ${env.VOICE_SAMPLE_RATE}Hz ${env.VOICE_BITS_PER_SAMPLE}-bit PCM LE`,
  voice_wav_capture_enabled: env.VOICE_SAVE_WAV,
  voice_asr_backend: env.VOICE_ASR_BACKEND,
  voice_sherpa_model: env.VOICE_ASR_BACKEND === "sherpa" ? env.VOICE_SHERPA_MODEL : null,
  voice_live_test_mode: env.VOICE_LIVE_TEST_MODE,
  assistant_enabled: env.ASSISTANT_ENABLED,
  assistant_model: env.ASSISTANT_ENABLED ? env.ASSISTANT_MODEL : null,
  telegram_enabled: Boolean(bot),
  device_auth_configured: Boolean(env.NEWO_DEVICE_SECRET),
}, "Newo cloud started");
