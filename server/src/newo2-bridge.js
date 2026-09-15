import { randomUUID, timingSafeEqual } from "node:crypto";
import { createWriteStream, openAsBlob } from "node:fs";
import { mkdir, readFile, readdir, unlink, writeFile } from "node:fs/promises";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { loadEnvFile } from "node:process";
import path from "node:path";

import Fastify from "fastify";
import WebSocket, { WebSocketServer } from "ws";

try { loadEnvFile(".env"); } catch (error) { if (error?.code !== "ENOENT") throw error; }

const env = {
  host: process.env.NEWO2_BRIDGE_HOST || "127.0.0.1",
  port: Number(process.env.NEWO2_BRIDGE_PORT || 8792),
  deviceId: process.env.NEWO2_DEVICE_ID || "newo2-01",
  deviceSecret: process.env.NEWO2_DEVICE_SECRET || "",
  adminSecret: process.env.NEWO2_ADMIN_SECRET || "",
  snapshotDirectory: process.env.NEWO2_SNAPSHOT_DIRECTORY || "data/newo2/snapshots",
  videoDirectory: process.env.NEWO2_VIDEO_DIRECTORY || "data/newo2/videos",
  retention: Math.max(10, Math.min(2000, Number(process.env.NEWO2_SNAPSHOT_RETENTION || 200))),
  telegramToken: process.env.TELEGRAM_BOT_TOKEN || "",
  telegramChatId: process.env.NEWO2_TELEGRAM_CHAT_ID || "",
  ffmpeg: process.env.NEWO2_FFMPEG || "ffmpeg",
  visionBaseUrl: process.env.NEWO2_VISION_BASE_URL || "http://127.0.0.1:8183/v1",
  visionModel: process.env.NEWO2_VISION_MODEL || "newoai-vision",
  publicVisionUrl: process.env.NEWO2_PUBLIC_VISION_URL || "https://smonitor.reitaard.de/vision",
};

if (env.deviceSecret.length < 24) throw new Error("NEWO2_DEVICE_SECRET must be at least 24 characters");
if (env.adminSecret.length < 24) throw new Error("NEWO2_ADMIN_SECRET must be at least 24 characters");
if (safeEqual(env.adminSecret, env.deviceSecret)) throw new Error("NEWO2_ADMIN_SECRET must differ from NEWO2_DEVICE_SECRET");
if (!Number.isInteger(env.port) || env.port < 1 || env.port > 65535) throw new Error("invalid NEWO2_BRIDGE_PORT");

const app = Fastify({ logger: true, bodyLimit: 512 * 1024 });
const wss = new WebSocketServer({ noServer: true, perMessageDeflate: false, maxPayload: 320 * 1024 });
const serialMonitorWss = new WebSocketServer({ noServer: true, perMessageDeflate: false, maxPayload: 8 * 1024 });
const pending = new Map();
let device = null;
let activeRecording = null;
let latestFrame = null;
const streamClients = new Set();
const pendingPhotos = new Map();

function safeEqual(left, right) {
  if (typeof left !== "string" || typeof right !== "string") return false;
  const a = Buffer.from(left); const b = Buffer.from(right);
  return a.length === b.length && timingSafeEqual(a, b);
}

function authorized(headers) {
  const id = headers["x-newo-device-id"];
  const auth = headers.authorization;
  const secret = typeof auth === "string" && auth.startsWith("Bearer ") ? auth.slice(7) : "";
  return safeEqual(id, env.deviceId) && safeEqual(secret, env.deviceSecret);
}

function adminAuthorized(headers) {
  const auth = headers.authorization;
  const secret = typeof auth === "string" && auth.startsWith("Bearer ") ? auth.slice(7) : "";
  return safeEqual(secret, env.adminSecret);
}

function rejectUpgrade(socket, status, text) {
  socket.write(`HTTP/1.1 ${status} ${text}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
  socket.destroy();
}

function settle(requestId, result) {
  const item = pending.get(requestId);
  if (!item) return false;
  pending.delete(requestId); clearTimeout(item.timer); item.resolve(result); return true;
}

function failSocketPending(ws) {
  for (const [id, item] of pending) if (item.ws === ws) settle(id, { kind: "disconnected" });
}

function sendRequest(type, expectedTypes, fields = {}) {
  if (!device || device.ws.readyState !== WebSocket.OPEN) return { kind: "offline" };
  const requestId = randomUUID();
  let resolve;
  const promise = new Promise((r) => { resolve = r; });
  // A snapshot includes capture, SD write and an HTTPS upload whose device-side
  // timeout is 10 seconds. Keep the command alive long enough to receive the
  // real result instead of returning a false 504 while the image succeeds.
  const timer = setTimeout(() => settle(requestId, { kind: "timeout" }), 15000); timer.unref();
  pending.set(requestId, { ws: device.ws, expectedTypes: new Set(expectedTypes), resolve, timer });
  try { device.ws.send(JSON.stringify({ type, request_id: requestId, ...fields })); }
  catch { settle(requestId, { kind: "send_error" }); }
  return { kind: "sent", requestId, promise };
}

async function requestOrHttpError(reply, type, expectedTypes, fields) {
  const sent = sendRequest(type, expectedTypes, fields);
  if (sent.kind === "offline") return reply.code(503).send({ error: "newo2_offline" });
  const result = await sent.promise;
  if (result.kind === "timeout") return reply.code(504).send({ error: "newo2_timeout" });
  if (result.kind !== "response") return reply.code(502).send({ error: result.kind });
  return reply.send(result.message);
}

async function rotateSnapshots() {
  const files = (await readdir(env.snapshotDirectory, { withFileTypes: true }))
    .filter((entry) => entry.isFile() && entry.name.endsWith(".jpg")).map((entry) => entry.name).sort();
  const excess = files.slice(0, Math.max(0, files.length - env.retention));
  await Promise.all(excess.map((name) => unlink(path.join(env.snapshotDirectory, name)).catch(() => {})));
}

async function telegramPhoto(buffer, filename, source, chatId = env.telegramChatId, caption) {
  if (!env.telegramToken || !chatId) return false;
  const form = new FormData();
  form.set("chat_id", chatId);
  form.set("caption", caption || `Newo2 ${source || "snapshot"}`);
  form.set("photo", new Blob([buffer], { type: "image/jpeg" }), filename);
  const response = await fetch(`https://api.telegram.org/bot${env.telegramToken}/sendPhoto`, { method: "POST", body: form });
  if (!response.ok) app.log.warn({ status: response.status }, "Newo2 Telegram photo failed");
  return response.ok;
}

async function telegramText(chatId, message) {
  if (!env.telegramToken || !chatId) return false;
  const response = await fetch(`https://api.telegram.org/bot${env.telegramToken}/sendMessage`, {
    method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ chat_id: chatId, text: message }),
  });
  return response.ok;
}

async function telegramVideo(filePath, chatId, caption) {
  if (!env.telegramToken || !chatId) return false;
  const form = new FormData();
  form.set("chat_id", chatId); form.set("supports_streaming", "true"); form.set("caption", caption);
  form.set("video", await openAsBlob(filePath, { type: "video/mp4" }), path.basename(filePath));
  const response = await fetch(`https://api.telegram.org/bot${env.telegramToken}/sendVideo`, { method: "POST", body: form });
  if (!response.ok) app.log.warn({ status: response.status }, "Newo2 Telegram video failed");
  return response.ok;
}

async function analyzeVision(jpeg, question) {
  const response = await fetch(`${env.visionBaseUrl}/chat/completions`, {
    method: "POST", headers: { "content-type": "application/json" }, signal: AbortSignal.timeout(30_000),
    body: JSON.stringify({ model: env.visionModel, max_tokens: 180, messages: [{ role: "user", content: [
      { type: "text", text: question || "Describe what the Newo2 camera can see." },
      { type: "image_url", image_url: { url: `data:image/jpeg;base64,${jpeg.toString("base64")}` } },
    ] }] }),
  });
  if (!response.ok) throw new Error(`vision_http_${response.status}`);
  const body = await response.json();
  return body?.choices?.[0]?.message?.content?.trim() || "Vision model returned no description.";
}

function transcode(input, output, fps) {
  return new Promise((resolve, reject) => {
    const child = spawn(env.ffmpeg, ["-y", "-f", "mjpeg", "-framerate", String(fps || 15), "-i", input,
      "-c:v", "libx264", "-preset", "veryfast", "-pix_fmt", "yuv420p", "-movflags", "+faststart", output],
    { stdio: ["ignore", "ignore", "pipe"] });
    let stderr = "";
    child.stderr.on("data", (chunk) => { if (stderr.length < 8192) stderr += chunk; });
    child.on("error", reject);
    child.on("exit", (code) => code === 0 ? resolve() : reject(new Error(`ffmpeg_${code}: ${stderr.slice(-1000)}`)));
  });
}

async function finalizeRecording(message) {
  const recording = activeRecording;
  if (!recording || recording.requestId !== message.request_id) return;
  activeRecording = null;
  await new Promise((resolve) => recording.stream.end(resolve));
  const durationSeconds = Math.max(0.001, Number(message.duration_ms || 0) / 1000);
  const measuredFps = message.frames > 0 ? Math.max(1, Math.min(30, message.frames / durationSeconds)) : recording.fps;
  if (!message.success || recording.frames === 0) {
    await telegramText(recording.chatId, `Newo2 recording stopped: ${message.reason || "recording failed"}. The partial SD file was preserved.`).catch(() => {});
    return;
  }
  try {
    await transcode(recording.rawPath, recording.mp4Path, measuredFps);
    await telegramVideo(recording.mp4Path, recording.chatId,
      `Newo2 recording · ${message.frames} frames · ${measuredFps.toFixed(1)} FPS`);
  } catch (error) {
    app.log.error({ error: error?.message }, "Newo2 recording finalization failed");
    await telegramText(recording.chatId, `Newo2 recorded ${message.frames} frames, but MP4 conversion failed.`).catch(() => {});
  }
}

function publishFrame(jpeg) {
  const header = `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${jpeg.length}\r\n\r\n`;
  for (const client of streamClients) {
    if (client.destroyed || client.writableLength > 512 * 1024) continue;
    client.write(header); client.write(jpeg); client.write("\r\n");
  }
}

const serialMonitorAssets = new Map([
  ["/smonitor2", ["../public/smonitor.html", "text/html; charset=utf-8"]],
  ["/smonitor2/", ["../public/smonitor.html", "text/html; charset=utf-8"]],
  ["/smonitor2/smonitor.css", ["../public/smonitor.css", "text/css; charset=utf-8"]],
  ["/smonitor2/smonitor.js", ["../public/smonitor.js", "text/javascript; charset=utf-8"]],
  ["/smonitor2/favicon.ico", ["../public/smonitor-favicon/favicon.ico", "image/x-icon"]],
  ["/smonitor2/favicon.svg", ["../public/smonitor-favicon/favicon.svg", "image/svg+xml"]],
  ["/smonitor2/favicon-96x96.png", ["../public/smonitor-favicon/favicon-96x96.png", "image/png"]],
  ["/smonitor2/apple-touch-icon.png", ["../public/smonitor-favicon/apple-touch-icon.png", "image/png"]],
  ["/smonitor2/web-app-manifest-192x192.png", ["../public/smonitor-favicon/web-app-manifest-192x192.png", "image/png"]],
  ["/smonitor2/web-app-manifest-512x512.png", ["../public/smonitor-favicon/web-app-manifest-512x512.png", "image/png"]],
  ["/smonitor2/site.webmanifest", ["../public/smonitor-favicon/site.webmanifest", "application/manifest+json"]],
]);
for (const [route, [relativePath, contentType]] of serialMonitorAssets) {
  app.get(route, async (_request, reply) => {
    const body = await readFile(new URL(relativePath, import.meta.url));
    return reply.header("Cache-Control", "no-store").type(contentType).send(body);
  });
}

function broadcastSerialMonitorStatus(fields) {
  const payload = JSON.stringify({ type: "monitor_status", ...fields });
  for (const client of serialMonitorWss.clients) if (client.readyState === WebSocket.OPEN) client.send(payload);
}

let serialMonitorGeneration = 0;
async function synchronizeSerialMonitor() {
  const generation = ++serialMonitorGeneration;
  const enabled = serialMonitorWss.clients.size > 0;
  const sent = sendRequest("serial_monitor_control", ["serial_monitor_ack"], { enabled });
  if (sent.kind === "offline") {
    broadcastSerialMonitorStatus({ state: "device_offline", enabled: false });
    return;
  }
  broadcastSerialMonitorStatus({ state: enabled ? "starting" : "stopping", enabled });
  const result = await sent.promise;
  if (generation !== serialMonitorGeneration) return;
  if (result.kind !== "response") return broadcastSerialMonitorStatus({ state: result.kind, enabled: false });
  broadcastSerialMonitorStatus({
    state: result.message.applied ? (result.message.enabled ? "streaming" : "stopped") : "rejected",
    enabled: result.message.enabled,
    capacity_bytes: result.message.capacity_bytes,
  });
}

app.addContentTypeParser("image/jpeg", { parseAs: "buffer", bodyLimit: 512 * 1024 }, (request, body, done) => done(null, body));

app.post("/newo2/snapshot", async (request, reply) => {
  if (!authorized(request.headers)) return reply.code(401).send({ error: "unauthorized" });
  const jpeg = request.body;
  if (!Buffer.isBuffer(jpeg) || jpeg.length < 4 || jpeg[0] !== 0xff || jpeg[1] !== 0xd8 || jpeg.at(-2) !== 0xff || jpeg.at(-1) !== 0xd9) {
    return reply.code(400).send({ error: "invalid_jpeg" });
  }
  const query = request.query ?? {};
  const source = typeof query.source === "string" && /^[a-z0-9_-]{1,16}$/i.test(query.source) ? query.source : "unknown";
  const sequence = Number.isInteger(Number(query.sequence)) ? Number(query.sequence) : 0;
  const requestId = typeof query.request_id === "string" && /^[a-z0-9-]{0,64}$/i.test(query.request_id) ? query.request_id : "";
  await mkdir(env.snapshotDirectory, { recursive: true });
  const filename = `${String(Date.now()).padStart(13, "0")}-${String(sequence).padStart(6, "0")}.jpg`;
  await writeFile(path.join(env.snapshotDirectory, filename), jpeg, { flag: "wx" });
  await rotateSnapshots();
  const requested = requestId ? pendingPhotos.get(requestId) : null;
  if (requestId) pendingPhotos.delete(requestId);
  const destinationChat = requested?.chatId || env.telegramChatId;
  const telegramQueued = Boolean(env.telegramToken && destinationChat);
  if (telegramQueued) {
    void telegramPhoto(jpeg, filename, source, destinationChat).then(async () => {
      if (requested?.question) await telegramText(destinationChat, await analyzeVision(jpeg, requested.question));
    }).catch((error) => {
      app.log.warn({ error: error?.message }, "Newo2 Telegram send failed");
    });
  }
  app.log.info({ source, sequence, request_id: requestId || null, bytes: jpeg.length, telegram_queued: telegramQueued }, "Newo2 snapshot accepted");
  return reply.code(201).send({ ok: true, filename, bytes: jpeg.length, telegram_queued: telegramQueued });
});

app.addHook("preHandler", async (request, reply) => {
  if (!request.url.startsWith("/newo2/admin/")) return;
  if (!adminAuthorized(request.headers)) return reply.code(401).send({ error: "unauthorized" });
});

app.get("/newo2/admin/status", async () => ({
  connected: Boolean(device && device.ws.readyState === WebSocket.OPEN),
  id: env.deviceId,
  connected_at: device?.connectedAt ?? null,
  last_seen: device?.lastSeen ?? null,
  hello: device?.hello ?? null,
  status: device?.status ?? null,
  recording: activeRecording ? { started_at: activeRecording.startedAt, frames: activeRecording.frames } : null,
  latest_frame: latestFrame ? { sequence: latestFrame.sequence, width: latestFrame.width, height: latestFrame.height, fps: latestFrame.fps, received_at: latestFrame.receivedAt } : null,
}));

app.post("/newo2/admin/camera", async (request, reply) => {
  if (typeof request.body?.enabled !== "boolean") return reply.code(400).send({ error: "enabled_boolean_required" });
  return requestOrHttpError(reply, "camera_control", ["control_ack"], { enabled: request.body.enabled });
});
app.post("/newo2/admin/motion", async (request, reply) => {
  if (typeof request.body?.enabled !== "boolean") return reply.code(400).send({ error: "enabled_boolean_required" });
  return requestOrHttpError(reply, "motion_control", ["control_ack"], { enabled: request.body.enabled });
});
app.post("/newo2/admin/snapshot", async (request, reply) => {
  const sent = sendRequest("snapshot_capture", ["snapshot_captured", "snapshot_error"]);
  if (sent.kind === "offline") return reply.code(503).send({ error: "newo2_offline" });
  const chatId = request.body?.chat_id ? String(request.body.chat_id) : "";
  const question = typeof request.body?.question === "string" ? request.body.question.trim().slice(0, 500) : "";
  if (chatId || question) pendingPhotos.set(sent.requestId, { chatId, question });
  const result = await sent.promise;
  if (result.kind !== "response") {
    pendingPhotos.delete(sent.requestId);
    return reply.code(result.kind === "timeout" ? 504 : 502).send({ error: `newo2_${result.kind}` });
  }
  return reply.send({ ...result.message, telegram_queued: Boolean(chatId) });
});
app.post("/newo2/admin/stream", async (request, reply) => {
  if (typeof request.body?.enabled !== "boolean") return reply.code(400).send({ error: "enabled_boolean_required" });
  return requestOrHttpError(reply, "stream_control", ["media_ack"], { enabled: request.body.enabled });
});
app.post("/newo2/admin/record", async (request, reply) => {
  if (activeRecording) return reply.code(409).send({ error: "recording_active" });
  const duration = request.body?.duration_seconds === undefined ? 30 : Number(request.body.duration_seconds);
  if (!Number.isSafeInteger(duration) || duration < 0 || duration > 0xffffffff) return reply.code(400).send({ error: "invalid_duration" });
  await mkdir(env.videoDirectory, { recursive: true });
  const id = `${Date.now()}-${randomUUID()}`;
  const rawPath = path.join(env.videoDirectory, `${id}.mjpeg`);
  const mp4Path = path.join(env.videoDirectory, `${id}.mp4`);
  const stream = createWriteStream(rawPath, { flags: "wx" });
  try { await once(stream, "open"); } catch { return reply.code(500).send({ error: "video_storage_unavailable" }); }
  const sent = sendRequest("record_start", ["media_ack"], { duration_seconds: duration });
  if (sent.kind === "offline") { stream.destroy(); return reply.code(503).send({ error: "newo2_offline" }); }
  activeRecording = { requestId: sent.requestId, rawPath, mp4Path, stream,
    chatId: request.body?.chat_id ? String(request.body.chat_id) : env.telegramChatId, startedAt: new Date().toISOString(), frames: 0, fps: 20 };
  const result = await sent.promise;
  if (result.kind !== "response" || !result.message.applied) {
    const failed = activeRecording; activeRecording = null;
    await new Promise((resolve) => failed.stream.end(resolve));
    return reply.code(result.kind === "timeout" ? 504 : 502).send({ error: result.kind === "response" ? "record_rejected" : `newo2_${result.kind}` });
  }
  activeRecording.fps = result.message.fps || 20;
  return reply.send({ ...result.message, duration_seconds: duration });
});
app.post("/newo2/admin/record/stop", async (request, reply) => requestOrHttpError(reply, "record_stop", ["media_ack"], {}));
app.post("/newo2/admin/settings", async (request, reply) => {
  const body = request.body ?? {};
  if (!body.resolution && !body.quality) return requestOrHttpError(reply, "settings_request", ["settings_ack"], {});
  const kind = body.resolution ? "resolution" : "quality";
  const raw = String(body[kind]).trim().toLowerCase();
  const match = raw.match(/^(photo|video)(?:=|\s+)([a-z0-9]+)$/);
  if (!match) return reply.code(400).send({ error: `use_${kind}_as_photo_or_video_then_value` });
  const value = kind === "quality" ? Number(match[2]) : match[2];
  if (kind === "quality" && (!Number.isInteger(value) || value < 4 || value > 32)) return reply.code(400).send({ error: "quality_must_be_4_to_32" });
  return requestOrHttpError(reply, "settings_control", ["settings_ack"], { setting: kind, target: match[1], value });
});
app.post("/newo2/admin/status/refresh", async (request, reply) => requestOrHttpError(reply, "status_request", ["status"], {}));

app.get("/vision", async (_request, reply) => reply.type("text/html; charset=utf-8").send(`<!doctype html><html><head><meta name="viewport" content="width=device-width"><title>Newo2 Vision</title><style>body{margin:0;background:#080b10;color:#e8eef7;font:16px system-ui;display:grid;place-items:center;min-height:100vh}main{width:min(96vw,1000px)}img{width:100%;border-radius:14px;background:#111;min-height:240px;object-fit:contain}p{opacity:.7}</style></head><body><main><h1>Newo2 Vision</h1><img src="./vision/stream" alt="Newo2 live camera"><p>Private Tailscale stream · ${env.deviceId}</p></main></body></html>`));
app.get("/vision/stream", async (request, reply) => {
  reply.hijack();
  reply.raw.writeHead(200, { "content-type": "multipart/x-mixed-replace; boundary=frame", "cache-control": "no-store", connection: "close" });
  streamClients.add(reply.raw);
  if (latestFrame) publishFrame(latestFrame.jpeg);
  request.raw.on("close", () => streamClients.delete(reply.raw));
});

app.server.on("upgrade", (request, socket, head) => {
  let pathname = "/";
  try { pathname = new URL(request.url ?? "/", "http://localhost").pathname; } catch { rejectUpgrade(socket, 400, "Bad Request"); return; }
  if (pathname === "/smonitor2/ws") {
    serialMonitorWss.handleUpgrade(request, socket, head, (ws) => serialMonitorWss.emit("connection", ws, request));
    return;
  }
  if (pathname !== "/newo2/device") { rejectUpgrade(socket, 404, "Not Found"); return; }
  if (!authorized(request.headers)) { rejectUpgrade(socket, 401, "Unauthorized"); return; }
  wss.handleUpgrade(request, socket, head, (ws) => wss.emit("connection", ws, request));
});

serialMonitorWss.on("connection", (ws) => {
  ws.send(JSON.stringify({ type: "monitor_status", state: device ? "connecting" : "device_offline", enabled: false,
    device: { connected: Boolean(device), id: env.deviceId, firmware: device?.hello?.firmware ?? null, last_seen: device?.lastSeen ?? null } }));
  ws.on("error", () => {});
  ws.on("close", () => { if (serialMonitorWss.clients.size === 0) void synchronizeSerialMonitor(); });
  if (serialMonitorWss.clients.size === 1) void synchronizeSerialMonitor();
});

wss.on("connection", (ws) => {
  if (device?.ws && device.ws.readyState === WebSocket.OPEN) device.ws.close(4001, "replaced");
  device = { ws, connectedAt: new Date().toISOString(), lastSeen: new Date().toISOString(), hello: null, status: null };
  app.log.info({ device_id: env.deviceId }, "Newo2 connected");
  ws.on("message", (raw, binary) => {
    if (binary) {
      const frame = Buffer.from(raw);
      const magic = frame.length >= 4 ? frame.subarray(0, 4).toString("ascii") : "";
      if (magic === "NSM2" && frame.length >= 12) {
        for (const client of serialMonitorWss.clients) {
          if (client.readyState === WebSocket.OPEN && client.bufferedAmount < 64 * 1024) client.send(frame, { binary: true });
        }
        return;
      }
      if (magic !== "N2JF" || frame.length < 24) return;
      const jpegLength = frame.readUInt32LE(20);
      if (jpegLength < 4 || jpegLength !== frame.length - 24) return;
      const jpeg = frame.subarray(24);
      if (jpeg[0] !== 0xff || jpeg[1] !== 0xd8 || jpeg.at(-2) !== 0xff || jpeg.at(-1) !== 0xd9) return;
      latestFrame = { jpeg: Buffer.from(jpeg), flags: frame[4], width: frame.readUInt16LE(6), height: frame.readUInt16LE(8),
        fps: frame[10], sequence: frame.readUInt32LE(12), timestampMs: frame.readUInt32LE(16), receivedAt: new Date().toISOString() };
      publishFrame(latestFrame.jpeg);
      if (activeRecording && (frame[4] & 2)) { activeRecording.stream.write(jpeg); activeRecording.frames += 1; }
      return;
    }
    if (raw.length > 32 * 1024) return;
    let message; try { message = JSON.parse(raw.toString("utf8")); } catch { return; }
    if (!message || typeof message.type !== "string") return;
    device.lastSeen = new Date().toISOString();
    if (message.type === "hello") {
      if (message.device !== env.deviceId) { ws.close(4003, "device mismatch"); return; }
      device.hello = message; ws.send(JSON.stringify({ type: "hello_ack" }));
    } else if (message.type === "status") {
      device.status = message;
    } else if (message.type === "motion_detected") {
      app.log.info({ sequence: message.sequence, confidence: message.confidence }, "Newo2 motion detected");
    } else if (message.type === "snapshot_captured" || message.type === "snapshot_error") {
      app.log.info({ type: message.type, source: message.source, sequence: message.sequence, uploaded: message.uploaded }, "Newo2 snapshot result");
    } else if (message.type === "record_complete") {
      void finalizeRecording(message);
    }
    if (typeof message.request_id === "string") {
      const item = pending.get(message.request_id);
      if (item?.ws === ws && item.expectedTypes.has(message.type)) settle(message.request_id, { kind: "response", message });
    }
  });
  ws.on("close", () => {
    failSocketPending(ws);
    if (device?.ws === ws) device = null;
    if (activeRecording) {
      const interrupted = activeRecording; activeRecording = null; interrupted.stream.end();
      void telegramText(interrupted.chatId, "Newo2 disconnected; the partial recording was preserved on the SD card and VPS.").catch(() => {});
    }
    broadcastSerialMonitorStatus({ state: "device_offline", enabled: false });
    app.log.info({ device_id: env.deviceId }, "Newo2 disconnected");
  });
  ws.on("error", () => {});
  if (serialMonitorWss.clients.size > 0) void synchronizeSerialMonitor();
});

await Promise.all([mkdir(env.snapshotDirectory, { recursive: true }), mkdir(env.videoDirectory, { recursive: true })]);
await app.listen({ host: env.host, port: env.port });
app.log.info({ host: env.host, port: env.port, device_id: env.deviceId, snapshot_directory: env.snapshotDirectory }, "Newo2 bridge started");
