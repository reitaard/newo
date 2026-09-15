import assert from "node:assert/strict";
import { once } from "node:events";
import { mkdtemp, readdir, rm } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";
import test from "node:test";

import WebSocket from "ws";

const deviceSecret = "device-secret-at-least-24-chars";
const adminSecret = "admin-secret-at-least-24-chars";
const serverDirectory = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

async function freePort() {
  const server = net.createServer();
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const port = server.address().port;
  server.close();
  await once(server, "close");
  return port;
}

async function waitForBridge(url, child) {
  for (let attempt = 0; attempt < 50; attempt += 1) {
    if (child.exitCode !== null) throw new Error(`bridge exited with ${child.exitCode}`);
    try {
      const response = await fetch(`${url}/newo2/admin/status`);
      if (response.status === 401) return;
    } catch {}
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error("bridge did not start");
}

test("Newo2 bridge protects admin controls and bounds snapshot retention", async (t) => {
  const directory = await mkdtemp(path.join(os.tmpdir(), "newo2-bridge-"));
  const port = await freePort();
  const base = `http://127.0.0.1:${port}`;
  const child = spawn(process.execPath, [path.join(serverDirectory, "src/newo2-bridge.js")], {
    cwd: serverDirectory,
    env: {
      ...process.env,
      NEWO2_BRIDGE_PORT: String(port),
      NEWO2_DEVICE_ID: "newo2-test",
      NEWO2_DEVICE_SECRET: deviceSecret,
      NEWO2_ADMIN_SECRET: adminSecret,
      NEWO2_SNAPSHOT_DIRECTORY: directory,
      NEWO2_SNAPSHOT_RETENTION: "10",
      TELEGRAM_BOT_TOKEN: "",
      NEWO2_TELEGRAM_CHAT_ID: "",
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  t.after(async () => {
    child.kill();
    if (child.exitCode === null) await once(child, "exit");
    await rm(directory, { recursive: true, force: true });
  });

  await waitForBridge(base, child);
  assert.equal((await fetch(`${base}/newo2/admin/status`)).status, 401);

  const adminHeaders = { authorization: `Bearer ${adminSecret}` };
  const status = await fetch(`${base}/newo2/admin/status`, { headers: adminHeaders });
  assert.equal(status.status, 200);
  assert.equal((await status.json()).connected, false);

  const ws = new WebSocket(`ws://127.0.0.1:${port}/newo2/device`, {
    headers: {
      "x-newo-device-id": "newo2-test",
      authorization: `Bearer ${deviceSecret}`,
    },
  });
  t.after(() => ws.close());
  await once(ws, "open");
  ws.on("message", (raw) => {
    const message = JSON.parse(raw.toString("utf8"));
    if (message.type === "camera_control") {
      ws.send(JSON.stringify({
        type: "control_ack",
        request_id: message.request_id,
        target: "camera",
        enabled: message.enabled,
        applied: true,
      }));
    }
  });

  const control = await fetch(`${base}/newo2/admin/camera`, {
    method: "POST",
    headers: { ...adminHeaders, "content-type": "application/json" },
    body: JSON.stringify({ enabled: true }),
  });
  assert.equal(control.status, 200);
  assert.equal((await control.json()).applied, true);

  const jpeg = new Uint8Array([0xff, 0xd8, 0xff, 0xd9]);
  for (let sequence = 1; sequence <= 12; sequence += 1) {
    const upload = await fetch(`${base}/newo2/snapshot?source=test&sequence=${sequence}`, {
      method: "POST",
      headers: {
        "content-type": "image/jpeg",
        "x-newo-device-id": "newo2-test",
        authorization: `Bearer ${deviceSecret}`,
      },
      body: jpeg,
    });
    assert.equal(upload.status, 201);
  }
  assert.equal((await readdir(directory)).length, 10);
});
