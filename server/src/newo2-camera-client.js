export function createNewo2CameraClient({ baseUrl, adminSecret, timeoutMs = 20_000 }) {
  const configured = Boolean(baseUrl && adminSecret);
  async function request(path, body, method = "POST") {
    if (!configured) throw new Error("Newo2 camera bridge is not configured");
    const response = await fetch(`${baseUrl.replace(/\/$/, "")}${path}`, {
      method,
      headers: { authorization: `Bearer ${adminSecret}`, ...(body === undefined ? {} : { "content-type": "application/json" }) },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: AbortSignal.timeout(timeoutMs),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(payload.error || `Newo2 bridge HTTP ${response.status}`);
    return payload;
  }
  async function photo(chatId, question = "") {
    const result = await request("/newo2/admin/snapshot", { chat_id: String(chatId), question });
    return { ...result, captured: result.type === "snapshot_captured" };
  }
  return {
    configured,
    status: () => request("/newo2/admin/status", undefined, "GET"),
    camera: (enabled) => request("/newo2/admin/camera", { enabled }),
    photo,
    stream: (enabled) => request("/newo2/admin/stream", { enabled }),
    record: (chatId, durationSeconds = 30) => request("/newo2/admin/record", { chat_id: String(chatId), duration_seconds: durationSeconds }),
    stopRecording: () => request("/newo2/admin/record/stop", {}),
    settings: (body) => request("/newo2/admin/settings", body),
    flip: (enabled) => request("/newo2/admin/settings", { flip: Boolean(enabled) }),
    mirror: (enabled) => request("/newo2/admin/settings", { mirror: Boolean(enabled) }),
  };
}
