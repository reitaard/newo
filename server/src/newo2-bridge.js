import { randomUUID, timingSafeEqual } from "node:crypto";
import { mkdir, readdir, unlink, writeFile } from "node:fs/promises";
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
  snapshotDirectory: process.env.NEWO2_SNAPSHOT_DIRECTORY || "data/newo2/snapshots",
  retention: Math.max(10, Math.min(2000, Number(process.env.NEWO2_SNAPSHOT_RETENTION || 200))),
  telegramToken: process.env.TELEGRAM_BOT_TOKEN || "",
  telegramChatId: process.env.NEWO2_TELEGRAM_CHAT_ID || "",
};

if (env.deviceSecret.length < 24) throw new Error("NEWO2_DEVICE_SECRET must be at least 24 characters");
if (!Number.isInteger(env.port) || env.port < 1 || env.port > 65535) throw new Error("invalid NEWO2_BRIDGE_PORT");

const app = Fastify({ logger: true, bodyLimit: 512 * 1024 });
const wss = new WebSocketServer({ noServer: true, perMessageDeflate: false, maxPayload: 32 * 1024 });
const pending = new Map();
let device = null;

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
  const timer = setTimeout(() => settle(requestId, { kind: "timeout" }), 5000); timer.unref();
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

async function telegramPhoto(buffer, filename, source) {
  if (!env.telegramToken || !env.telegramChatId) return false;
  const form = new FormData();
  form.set("chat_id", env.telegramChatId);
  form.set("caption", `Newo2 ${source || "snapshot"}`);
  form.set("photo", new Blob([buffer], { type: "image/jpeg" }), filename);
  const response = await fetch(`https://api.telegram.org/bot${env.telegramToken}/sendPhoto`, { method: "POST", body: form });
  if (!response.ok) app.log.warn({ status: response.status }, "Newo2 Telegram photo failed");
  return response.ok;
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
  const telegram = await telegramPhoto(jpeg, filename, source).catch((error) => { app.log.warn({ error: error?.message }, "Newo2 Telegram send failed"); return false; });
  app.log.info({ source, sequence, request_id: requestId || null, bytes: jpeg.length, telegram }, "Newo2 snapshot accepted");
  return reply.code(201).send({ ok: true, filename, bytes: jpeg.length, telegram });
});

app.get("/newo2/admin/status", async () => ({
  connected: Boolean(device && device.ws.readyState === WebSocket.OPEN),
  id: env.deviceId,
  connected_at: device?.connectedAt ?? null,
  last_seen: device?.lastSeen ?? null,
  hello: device?.hello ?? null,
  status: device?.status ?? null,
}));

app.post("/newo2/admin/camera", async (request, reply) => {
  if (typeof request.body?.enabled !== "boolean") return reply.code(400).send({ error: "enabled_boolean_required" });
  return requestOrHttpError(reply, "camera_control", ["control_ack"], { enabled: request.body.enabled });
});
app.post("/newo2/admin/motion", async (request, reply) => {
  if (typeof request.body?.enabled !== "boolean") return reply.code(400).send({ error: "enabled_boolean_required" });
  return requestOrHttpError(reply, "motion_control", ["control_ack"], { enabled: request.body.enabled });
});
app.post("/newo2/admin/snapshot", async (request, reply) => requestOrHttpError(reply, "snapshot_capture", ["snapshot_captured", "snapshot_error"], {}));
app.post("/newo2/admin/status/refresh", async (request, reply) => requestOrHttpError(reply, "status_request", ["status"], {}));

app.server.on("upgrade", (request, socket, head) => {
  let pathname = "/";
  try { pathname = new URL(request.url ?? "/", "http://localhost").pathname; } catch { rejectUpgrade(socket, 400, "Bad Request"); return; }
  if (pathname !== "/newo2/device") { rejectUpgrade(socket, 404, "Not Found"); return; }
  if (!authorized(request.headers)) { rejectUpgrade(socket, 401, "Unauthorized"); return; }
  wss.handleUpgrade(request, socket, head, (ws) => wss.emit("connection", ws, request));
});

wss.on("connection", (ws) => {
  if (device?.ws && device.ws.readyState === WebSocket.OPEN) device.ws.close(4001, "replaced");
  device = { ws, connectedAt: new Date().toISOString(), lastSeen: new Date().toISOString(), hello: null, status: null };
  app.log.info({ device_id: env.deviceId }, "Newo2 connected");
  ws.on("message", (raw, binary) => {
    if (binary || raw.length > 32 * 1024) return;
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
    }
    if (typeof message.request_id === "string") {
      const item = pending.get(message.request_id);
      if (item?.ws === ws && item.expectedTypes.has(message.type)) settle(message.request_id, { kind: "response", message });
    }
  });
  ws.on("close", () => { failSocketPending(ws); if (device?.ws === ws) device = null; app.log.info({ device_id: env.deviceId }, "Newo2 disconnected"); });
  ws.on("error", () => {});
});

await mkdir(env.snapshotDirectory, { recursive: true });
await app.listen({ host: env.host, port: env.port });
app.log.info({ host: env.host, port: env.port, device_id: env.deviceId, snapshot_directory: env.snapshotDirectory }, "Newo2 bridge started");
