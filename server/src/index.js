import { timingSafeEqual, randomUUID } from "node:crypto";
import { loadEnvFile } from "node:process";

import Fastify from "fastify";
import { Bot, webhookCallback } from "grammy";
import WebSocket, { WebSocketServer } from "ws";
import { z } from "zod";

try {
  loadEnvFile(".env");
} catch (error) {
  if (error?.code !== "ENOENT") {
    throw error;
  }
}

const emptyToUndefined = (value) => {
  if (typeof value !== "string") return value;
  const trimmed = value.trim();
  return trimmed.length === 0 ? undefined : trimmed;
};

const EnvSchema = z.object({
  HOST: z.preprocess(emptyToUndefined, z.string().default("127.0.0.1")),
  PORT: z.preprocess(emptyToUndefined, z.coerce.number().int().min(1).max(65535).default(8788)),
  PUBLIC_BASE_URL: z.preprocess(
    emptyToUndefined,
    z.string().url().default("https://newo.reitaard.de"),
  ),
  TELEGRAM_BOT_TOKEN: z.preprocess(emptyToUndefined, z.string().optional()),
  TELEGRAM_WEBHOOK_SECRET: z.preprocess(emptyToUndefined, z.string().min(16).optional()),
  TELEGRAM_ALLOWED_USER_IDS: z.preprocess(emptyToUndefined, z.string().optional()),
  TELEGRAM_ALLOWED_CHAT_IDS: z.preprocess(emptyToUndefined, z.string().optional()),
  NEWO_DEVICE_ID: z.preprocess(emptyToUndefined, z.string().min(1).default("newo-01")),
  NEWO_DEVICE_SECRET: z.preprocess(emptyToUndefined, z.string().min(24).optional()),
});

const env = EnvSchema.parse(process.env);

if (env.TELEGRAM_BOT_TOKEN && !env.TELEGRAM_WEBHOOK_SECRET) {
  throw new Error("TELEGRAM_WEBHOOK_SECRET is required when TELEGRAM_BOT_TOKEN is set");
}

const parseIdSet = (value) =>
  new Set(
    (value ?? "")
      .split(",")
      .map((item) => item.trim())
      .filter(Boolean),
  );

const allowedUserIds = parseIdSet(env.TELEGRAM_ALLOWED_USER_IDS);
const allowedChatIds = parseIdSet(env.TELEGRAM_ALLOWED_CHAT_IDS);

const app = Fastify({
  logger: true,
  trustProxy: true,
  bodyLimit: 256 * 1024,
});

const wss = new WebSocketServer({
  noServer: true,
  perMessageDeflate: false,
  maxPayload: 64 * 1024,
});

const devices = new Map();

const DeviceMessageSchema = z.discriminatedUnion("type", [
  z
    .object({
      type: z.literal("hello"),
      device: z.string().min(1),
      firmware: z.string().optional(),
      chip: z.string().optional(),
    })
    .passthrough(),
  z
    .object({
      type: z.literal("pong"),
      request_id: z.string().optional(),
      uptime_ms: z.number().nonnegative().optional(),
      rssi: z.number().optional(),
    })
    .passthrough(),
  z
    .object({
      type: z.literal("status"),
      request_id: z.string().optional(),
      uptime_ms: z.number().nonnegative().optional(),
      rssi: z.number().optional(),
      free_heap: z.number().nonnegative().optional(),
      free_psram: z.number().nonnegative().optional(),
    })
    .passthrough(),
]);

function safeEqual(left, right) {
  if (typeof left !== "string" || typeof right !== "string") return false;
  const leftBuffer = Buffer.from(left);
  const rightBuffer = Buffer.from(right);
  return leftBuffer.length === rightBuffer.length && timingSafeEqual(leftBuffer, rightBuffer);
}

function rejectUpgrade(socket, statusCode, statusText) {
  socket.write(
    `HTTP/1.1 ${statusCode} ${statusText}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`,
  );
  socket.destroy();
}

function getDeviceSnapshot() {
  const device = devices.get(env.NEWO_DEVICE_ID);
  if (!device || device.ws.readyState !== WebSocket.OPEN) {
    return {
      connected: false,
      id: env.NEWO_DEVICE_ID,
      last_seen: device?.lastSeen ?? null,
      status: device?.status ?? null,
    };
  }

  return {
    connected: true,
    id: env.NEWO_DEVICE_ID,
    connected_at: device.connectedAt,
    last_seen: device.lastSeen,
    status: device.status ?? null,
  };
}

function sendToDevice(message) {
  const device = devices.get(env.NEWO_DEVICE_ID);
  if (!device || device.ws.readyState !== WebSocket.OPEN) return false;
  device.ws.send(JSON.stringify(message));
  return true;
}

app.get("/", async () => ({
  service: "newo-cloud",
  status: "ok",
}));

app.get("/health", async () => ({
  status: "ok",
  service: "newo-cloud",
  uptime_s: Math.floor(process.uptime()),
  telegram_enabled: Boolean(env.TELEGRAM_BOT_TOKEN),
  device: getDeviceSnapshot(),
}));

let bot = null;
if (env.TELEGRAM_BOT_TOKEN) {
  bot = new Bot(env.TELEGRAM_BOT_TOKEN);

  bot.use(async (ctx, next) => {
    const userId = ctx.from?.id?.toString();
    const chatId = ctx.chat?.id?.toString();
    const allowed =
      (userId && allowedUserIds.has(userId)) ||
      (chatId && allowedChatIds.has(chatId));

    if (!allowed) {
      app.log.warn(
        { user_id: userId ?? null, chat_id: chatId ?? null },
        "Rejected Telegram update outside allowlist",
      );
      return;
    }

    await next();
  });

  bot.command("start", async (ctx) => {
    await ctx.reply("Newo cloud is online.");
  });

  bot.command("status", async (ctx) => {
    const snapshot = getDeviceSnapshot();
    if (!snapshot.connected) {
      await ctx.reply(`Newo ${snapshot.id}: offline`);
      return;
    }

    const rssi = snapshot.status?.rssi;
    const suffix = typeof rssi === "number" ? ` | RSSI ${rssi} dBm` : "";
    await ctx.reply(`Newo ${snapshot.id}: online${suffix}`);
  });

  bot.command("ping", async (ctx) => {
    const requestId = randomUUID();
    if (!sendToDevice({ type: "ping", request_id: requestId, ts: Date.now() })) {
      await ctx.reply("Newo is offline.");
      return;
    }
    await ctx.reply("Ping sent to Newo.");
  });

  app.post(
    "/telegram/webhook",
    webhookCallback(bot, "fastify", {
      secretToken: env.TELEGRAM_WEBHOOK_SECRET,
      onTimeout: "return",
      timeoutMilliseconds: 9_000,
    }),
  );
}

app.server.on("upgrade", (request, socket, head) => {
  let pathname;
  try {
    pathname = new URL(request.url ?? "/", "http://localhost").pathname;
  } catch {
    rejectUpgrade(socket, 400, "Bad Request");
    return;
  }

  if (pathname !== "/device") {
    rejectUpgrade(socket, 404, "Not Found");
    return;
  }

  if (!env.NEWO_DEVICE_SECRET) {
    app.log.error("Rejected device connection because NEWO_DEVICE_SECRET is not configured");
    rejectUpgrade(socket, 503, "Service Unavailable");
    return;
  }

  const deviceId = request.headers["x-newo-device-id"];
  const authorization = request.headers.authorization;
  const presentedSecret =
    typeof authorization === "string" && authorization.startsWith("Bearer ")
      ? authorization.slice("Bearer ".length)
      : undefined;

  if (
    !safeEqual(deviceId, env.NEWO_DEVICE_ID) ||
    !safeEqual(presentedSecret, env.NEWO_DEVICE_SECRET)
  ) {
    app.log.warn({ device_id: deviceId ?? null }, "Rejected unauthenticated Newo device");
    rejectUpgrade(socket, 401, "Unauthorized");
    return;
  }

  wss.handleUpgrade(request, socket, head, (ws) => {
    wss.emit("connection", ws, request, deviceId);
  });
});

wss.on("connection", (ws, request, deviceId) => {
  const previous = devices.get(deviceId);
  if (previous?.ws.readyState === WebSocket.OPEN) {
    previous.ws.close(4001, "replaced by new connection");
  }

  const state = {
    ws,
    connectedAt: new Date().toISOString(),
    lastSeen: new Date().toISOString(),
    status: previous?.status ?? null,
    isAlive: true,
  };
  devices.set(deviceId, state);

  app.log.info({ device_id: deviceId }, "Newo device connected");

  ws.on("pong", () => {
    state.isAlive = true;
    state.lastSeen = new Date().toISOString();
  });

  ws.on("message", (raw, isBinary) => {
    state.lastSeen = new Date().toISOString();

    if (isBinary) {
      app.log.warn({ device_id: deviceId }, "Ignoring unexpected binary device message");
      return;
    }

    let json;
    try {
      json = JSON.parse(raw.toString("utf8"));
    } catch {
      app.log.warn({ device_id: deviceId }, "Ignoring invalid JSON from device");
      return;
    }

    const parsed = DeviceMessageSchema.safeParse(json);
    if (!parsed.success) {
      app.log.warn(
        { device_id: deviceId, issues: parsed.error.issues },
        "Ignoring invalid device message",
      );
      return;
    }

    const message = parsed.data;
    if (message.type === "hello" && message.device !== deviceId) {
      app.log.warn(
        { authenticated_device: deviceId, claimed_device: message.device },
        "Device hello identity mismatch",
      );
      ws.close(4003, "device identity mismatch");
      return;
    }

    if (message.type === "status" || message.type === "pong") {
      state.status = {
        ...(state.status ?? {}),
        ...message,
        received_at: state.lastSeen,
      };
    }

    app.log.info({ device_id: deviceId, type: message.type }, "Device message received");
  });

  ws.on("close", (code, reason) => {
    const current = devices.get(deviceId);
    if (current?.ws === ws) {
      current.lastSeen = new Date().toISOString();
    }
    app.log.info(
      { device_id: deviceId, code, reason: reason.toString() },
      "Newo device disconnected",
    );
  });

  ws.on("error", (error) => {
    app.log.warn({ device_id: deviceId, err: error }, "Newo WebSocket error");
  });

  ws.send(
    JSON.stringify({
      type: "hello_ack",
      device: deviceId,
      server_time: new Date().toISOString(),
    }),
  );
});

const heartbeatTimer = setInterval(() => {
  for (const [deviceId, state] of devices) {
    if (state.ws.readyState !== WebSocket.OPEN) continue;

    if (!state.isAlive) {
      app.log.warn({ device_id: deviceId }, "Terminating stale Newo WebSocket");
      state.ws.terminate();
      continue;
    }

    state.isAlive = false;
    state.ws.ping();
  }
}, 30_000);
heartbeatTimer.unref();

async function shutdown(signal) {
  app.log.info({ signal }, "Shutting down Newo cloud");
  clearInterval(heartbeatTimer);

  for (const state of devices.values()) {
    if (state.ws.readyState === WebSocket.OPEN) {
      state.ws.close(1001, "server shutting down");
    }
  }

  await app.close();
  process.exit(0);
}

process.once("SIGINT", () => void shutdown("SIGINT"));
process.once("SIGTERM", () => void shutdown("SIGTERM"));

await app.listen({ host: env.HOST, port: env.PORT });

app.log.info(
  {
    bind: `${env.HOST}:${env.PORT}`,
    public_base_url: env.PUBLIC_BASE_URL,
    websocket_path: "/device",
    telegram_enabled: Boolean(bot),
    device_auth_configured: Boolean(env.NEWO_DEVICE_SECRET),
  },
  "Newo cloud started",
);
