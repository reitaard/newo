const FRIENDLY_ERRORS = {
  camera_off: "Camera gate is OFF — use /cam_on first.",
  recording_active: "A recording is already active.",
  stream_active: "Live stream is active — stop it before recording.",
  media_busy: "Camera media path is busy — stop the active stream or recording first.",
  record_rejected: "Recording was rejected by Newo2.",
  snapshot_rejected: "Photo capture was rejected by Newo2.",
  stream_rejected: "Live stream was rejected by Newo2.",
  newo2_offline: "Newo2 is offline.",
  newo2_timeout: "Newo2 did not reply in time.",
};

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
    if (!response.ok) {
      const code = String(payload.error || `Newo2 bridge HTTP ${response.status}`);
      const error = new Error(FRIENDLY_ERRORS[code] || payload.message || code);
      error.code = code;
      error.status = response.status;
      throw error;
    }
    return payload;
  }

  async function photo(chatId, question = "") {
    const result = await request("/newo2/admin/snapshot", { chat_id: String(chatId), question });
    if (result.type !== "snapshot_captured") throw new Error("Photo capture failed on Newo2.");
    return { ...result, captured: true };
  }

  async function stream(enabled) {
    const result = await request("/newo2/admin/stream", { enabled });
    if (!result.applied) throw new Error(enabled
      ? "Live stream was rejected — camera gate may be OFF or recording is active."
      : "Live stream stop was rejected.");
    return result;
  }

  async function camera(enabled) {
    const result = await request("/newo2/admin/camera", { enabled });
    if (!result.applied) throw new Error(`Camera gate could not be turned ${enabled ? "ON" : "OFF"}.`);
    return result;
  }

  return {
    configured,
    status: () => request("/newo2/admin/status", undefined, "GET"),
    camera,
    photo,
    stream,
    record: (chatId, durationSeconds = 30) => request("/newo2/admin/record", { chat_id: String(chatId), duration_seconds: durationSeconds }),
    stopRecording: () => request("/newo2/admin/record/stop", {}),
    settings: (body) => request("/newo2/admin/settings", body),
    flip: (enabled) => request("/newo2/admin/settings", { flip: Boolean(enabled) }),
    mirror: (enabled) => request("/newo2/admin/settings", { mirror: Boolean(enabled) }),
  };
}
