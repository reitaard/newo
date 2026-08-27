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
const pendingRequests = new Map();
const DEVICE_REQUEST_TIMEOUT_MS = 5_000;
const OFFLINE_GRACE_MS = 12_000;
let bot = null;
let shuttingDown = false;

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
      ssid: z.string().optional(),
    })
    .passthrough(),
  z
    .object({
      type: z.literal("status"),
      request_id: z.string().optional(),
      uptime_ms: z.number().nonnegative().optional(),
      rssi: z.number().optional(),
      ssid: z.string().optional(),
      free_heap: z.number().nonnegative().optional(),
      free_psram: z.number().nonnegative().optional(),
    })
    .passthrough(),
  z
    .object({
      type: z.literal("reboot_ack"),
      request_id: z.string().optional(),
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
  const connected = device?.ws?.readyState === WebSocket.OPEN;

  return {
    connected,
    id: env.NEWO_DEVICE_ID,
    connected_at: device?.connectedAt ?? null,
    last_seen: device?.lastSeen ?? null,
    hello: device?.hello ?? null,
    status: device?.status ?? null,
  };
}

function getConnectedDeviceState() {
  const device = devices.get(env.NEWO_DEVICE_ID);
  return device?.ws?.readyState === WebSocket.OPEN ? device : null;
}

function settlePendingRequest(requestId, result) {
  const pending = pendingRequests.get(requestId);
  if (!pending) return false;

  pendingRequests.delete(requestId);
  clearTimeout(pending.timer);
  pending.resolve(result);
  return true;
}

function createRequestId() {
  let requestId;
  do {
    requestId = randomUUID();
  } while (pendingRequests.has(requestId));
  return requestId;
}

function sendDeviceRequest(requestType, responseType) {
  const device = getConnectedDeviceState();
  if (!device) return { kind: "offline" };

  const requestId = createRequestId();
  const startedAt = Date.now();
  let timer;
  let resolveRequest;
  const promise = new Promise((resolve) => {
    resolveRequest = resolve;
  });

  timer = setTimeout(() => {
    settlePendingRequest(requestId, { kind: "timeout" });
  }, DEVICE_REQUEST_TIMEOUT_MS);
  timer.unref();
  pendingRequests.set(requestId, {
    deviceId: env.NEWO_DEVICE_ID,
    ws: device.ws,
    requestType,
    responseType,
    startedAt,
    timer,
    resolve: resolveRequest,
  });

  try {
    device.ws.send(
      JSON.stringify({
        type: requestType,
        request_id: requestId,
      }),
      (error) => {
        if (error) {
          settlePendingRequest(requestId, { kind: "send_error" });
        }
      },
    );
  } catch {
    settlePendingRequest(requestId, { kind: "send_error" });
  }

  return { kind: "sent", requestId, promise };
}

function resolvePendingResponse(deviceId, ws, message) {
  if (!message.request_id) return false;

  const pending = pendingRequests.get(message.request_id);
  if (
    !pending ||
    pending.deviceId !== deviceId ||
    pending.ws !== ws ||
    pending.responseType !== message.type
  ) {
    return false;
  }

  return settlePendingRequest(message.request_id, {
    kind: "response",
    message,
    elapsedMs: Math.max(0, Date.now() - pending.startedAt),
  });
}

function failPendingRequestsForDevice(deviceId, ws, kind = "disconnected") {
  for (const [requestId, pending] of pendingRequests) {
    if (pending.deviceId === deviceId && pending.ws === ws) {
      settlePendingRequest(requestId, { kind });
    }
  }
}

function formatDuration(milliseconds) {
  if (!Number.isFinite(milliseconds) || milliseconds < 0) return "unknown";

  let seconds = Math.floor(milliseconds / 1_000);
  if (seconds < 1) return `${Math.floor(milliseconds)} ms`;

  const days = Math.floor(seconds / 86_400);
  seconds %= 86_400;
  const hours = Math.floor(seconds / 3_600);
  seconds %= 3_600;
  const minutes = Math.floor(seconds / 60);
  seconds %= 60;

  const parts = [];
  if (days) parts.push(`${days}d`);
  if (hours) parts.push(`${hours}h`);
  if (minutes) parts.push(`${minutes}m`);
  if (seconds || parts.length === 0) parts.push(`${seconds}s`);
  return parts.join(" ");
}

function formatDeviceStatus(snapshot, source = "live") {
  const status = snapshot.status ?? {};
  const hello = snapshot.hello ?? {};
  const suffix = source === "cached" ? " (cached)" : "";
  const lines = [`Newo ${snapshot.connected ? "online" : "offline"}${suffix}`];

  if (!snapshot.connected) {
    return lines.join("\n");
  }

  lines.push(
    `Wi-Fi: ${status.ssid ?? "unknown"} (${typeof status.rssi === "number" ? `${status.rssi} dBm` : "unknown"})`,
    `Uptime: ${formatDuration(status.uptime_ms)}`,
    `Firmware: ${hello.firmware ?? "unknown"}`,
  );
  return lines.join("\n");
}

function cancelOfflineTimer(state) {
  if (state?.offlineTimer) {
    clearTimeout(state.offlineTimer);
    state.offlineTimer = null;
  }
}

function sendConnectivityNotification(text) {
  if (!bot || allowedChatIds.size === 0) return;

  for (const chatId of allowedChatIds) {
    void Promise.resolve()
      .then(() => bot.api.sendMessage(chatId, text))
      .catch(() => {
        app.log.warn({ chat_id: chatId }, "Failed to send Newo connectivity notification");
      });
  }
}

function scheduleOfflineNotification(deviceId, state, ws) {
  if (shuttingDown || !state.hasBeenConnected) return;

  cancelOfflineTimer(state);
  state.offlineSince = Date.now();
  state.offlineNotified = false;
  state.offlineTimer = setTimeout(() => {
    state.offlineTimer = null;
    const current = devices.get(deviceId);
    if (
      shuttingDown ||
      current !== state ||
      current.ws !== ws ||
      current.ws.readyState === WebSocket.OPEN
    ) {
      return;
    }

    state.offlineNotified = true;
    sendConnectivityNotification("Newo went offline.");
  }, OFFLINE_GRACE_MS);
  state.offlineTimer.unref();
}

app.get("/", async () => ({
  service: "newo-cloud",
  status: "ok",
}));

app.get("/health", async () => {
  const device = getDeviceSnapshot();
  return {
    status: "ok",
    service: "newo-cloud",
    uptime_s: Math.floor(process.uptime()),
    telegram_enabled: Boolean(env.TELEGRAM_BOT_TOKEN),
    device: {
      connected: device.connected,
      id: device.id,
      connected_at: device.connected_at,
      last_seen: device.last_seen,
      firmware: device.hello?.firmware ?? null,
      chip: device.hello?.chip ?? null,
    },
  };
});

const TELEGRAM_COMMANDS = [
  { command: "status", description: "Device status" },
  { command: "reboot", description: "Restart Newo" },
];

async function handleStatusCommand(ctx) {
  const request = sendDeviceRequest("status_request", "status");
  if (request.kind === "offline") {
    await ctx.reply(`Newo ${env.NEWO_DEVICE_ID}: offline`);
    return;
  }

  const result = await request.promise;
  if (result.kind === "response") {
    await ctx.reply(formatDeviceStatus(getDeviceSnapshot()));
    return;
  }

  if (result.kind === "timeout") {
    const snapshot = getDeviceSnapshot();
    if (snapshot.status) {
      await ctx.reply(formatDeviceStatus(snapshot, "cached"));
    } else {
      await ctx.reply("Newo did not reply within 5 seconds.");
    }
    return;
  }

  await ctx.reply(
    getConnectedDeviceState() ? "Newo did not reply within 5 seconds." : `Newo ${env.NEWO_DEVICE_ID}: offline`,
  );
}

async function handlePingCommand(ctx) {
  const request = sendDeviceRequest("ping", "pong");
  if (request.kind === "offline") {
    await ctx.reply("Newo is offline.");
    return;
  }

  const result = await request.promise;
  if (result.kind === "response") {
    await ctx.reply(`Newo replied in ${result.elapsedMs} ms`);
    return;
  }

  if (result.kind === "timeout") {
    await ctx.reply("Newo did not reply within 5 seconds.");
    return;
  }

  await ctx.reply(
    getConnectedDeviceState() ? "Newo did not reply within 5 seconds." : "Newo is offline.",
  );
}

async function handleRebootCommand(ctx) {
  const request = sendDeviceRequest("reboot", "reboot_ack");
  if (request.kind === "offline") {
    await ctx.reply("Newo is offline.");
    return;
  }

  const result = await request.promise;
  if (result.kind === "response") {
    await ctx.reply("Restarting Newo.");
    return;
  }

  if (result.kind === "timeout") {
    await ctx.reply("Newo did not acknowledge the restart.");
    return;
  }

  await ctx.reply(
    getConnectedDeviceState() ? "Newo did not acknowledge the restart." : "Newo is offline.",
  );
}

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
    const state = getDeviceSnapshot().connected ? "online" : "offline";
    await ctx.reply(`Newo is ${state}.`);
  });

  bot.command("status", handleStatusCommand);
  bot.command("ping", handlePingCommand);
  bot.command("reboot", handleRebootCommand);

  void bot.api.setMyCommands(TELEGRAM_COMMANDS).catch(() => {
    app.log.warn("Failed to register the Telegram command menu");
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
  const reconnectingAfterNotifiedOffline = Boolean(
    previous?.hasBeenConnected &&
      previous.offlineNotified &&
      previous.offlineSince !== null,
  );
  const offlineDuration = reconnectingAfterNotifiedOffline
    ? Math.max(0, Date.now() - previous.offlineSince)
    : 0;

  cancelOfflineTimer(previous);
  if (previous?.ws.readyState === WebSocket.OPEN) {
    failPendingRequestsForDevice(deviceId, previous.ws, "disconnected");
    previous.ws.close(4001, "replaced by new connection");
  }

  const state = {
    ws,
    connectedAt: new Date().toISOString(),
    lastSeen: new Date().toISOString(),
    hello: previous?.hello ?? null,
    status: previous?.status ?? null,
    hasBeenConnected: previous?.hasBeenConnected ?? true,
    offlineSince: null,
    offlineNotified: false,
    offlineTimer: null,
    isAlive: true,
  };
  devices.set(deviceId, state);

  if (reconnectingAfterNotifiedOffline) {
    sendConnectivityNotification(
      `Newo is back online after ${formatDuration(offlineDuration)}.`,
    );
  }

  app.log.info({ device_id: deviceId }, "Newo device connected");

  ws.on("pong", () => {
    state.isAlive = true;
    state.lastSeen = new Date().toISOString();
  });

  ws.on("message", (raw, isBinary) => {
    const current = devices.get(deviceId);
    if (current !== state || current.ws !== ws) return;

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

    if (message.type === "hello") {
      state.hello = {
        device: message.device,
        firmware: message.firmware ?? null,
        chip: message.chip ?? null,
        received_at: state.lastSeen,
      };
    }

    if (message.type === "status" || message.type === "pong") {
      state.status = {
        ...(state.status ?? {}),
        ...message,
        received_at: state.lastSeen,
      };
    }

    resolvePendingResponse(deviceId, ws, message);
    app.log.info({ device_id: deviceId, type: message.type }, "Device message received");
  });

  ws.on("close", (code, reason) => {
    const current = devices.get(deviceId);
    failPendingRequestsForDevice(deviceId, ws, "disconnected");
    if (current?.ws === ws) {
      current.lastSeen = new Date().toISOString();
      scheduleOfflineNotification(deviceId, current, ws);
    }
    app.log.info(
      { device_id: deviceId, code, reason: reason.toString() },
      "Newo device disconnected",
    );
  });

  ws.on("error", () => {
    app.log.warn({ device_id: deviceId }, "Newo WebSocket error");
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

async function closeDeviceSocket(ws) {
  if (ws.readyState === WebSocket.CLOSED) return;

  await new Promise((resolve) => {
    let finished = false;
    const finish = () => {
      if (finished) return;
      finished = true;
      clearTimeout(timeout);
      resolve();
    };
    const timeout = setTimeout(() => {
      ws.terminate();
      finish();
    }, 1_000);
    timeout.unref();

    ws.once("close", finish);
    ws.once("error", finish);
    try {
      ws.close(1001, "server shutting down");
    } catch {
      ws.terminate();
      finish();
    }
  });
}

async function shutdown(signal) {
  if (shuttingDown) return;
  shuttingDown = true;
  app.log.info({ signal }, "Shutting down Newo cloud");
  clearInterval(heartbeatTimer);

  for (const [requestId] of pendingRequests) {
    settlePendingRequest(requestId, { kind: "shutdown" });
  }
  for (const state of devices.values()) {
    cancelOfflineTimer(state);
  }

  await Promise.all(
    [...devices.values()]
      .filter((state) => state.ws.readyState !== WebSocket.CLOSED)
      .map((state) => closeDeviceSocket(state.ws)),
  );

  await new Promise((resolve) => {
    try {
      wss.close(() => resolve());
    } catch {
      resolve();
    }
  });
  await app.close();
  process.exitCode = 0;
}

function handleShutdownSignal(signal) {
  void shutdown(signal).catch(() => {
    process.exitCode = 1;
  });
}

process.once("SIGINT", () => handleShutdownSignal("SIGINT"));
process.once("SIGTERM", () => handleShutdownSignal("SIGTERM"));

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
