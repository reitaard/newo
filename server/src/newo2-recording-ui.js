const escapeHtml = (value) => String(value ?? "").replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;");

export function formatBytes(value) {
  const bytes = Number(value);
  if (!Number.isFinite(bytes) || bytes < 0) return "n/a";
  if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(2)} GB`;
  if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(2)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${Math.round(bytes)} B`;
}

export function formatDuration(milliseconds) {
  const ms = Number(milliseconds);
  if (!Number.isFinite(ms) || ms < 0) return "n/a";
  const seconds = ms / 1000;
  if (seconds < 60) return `${seconds.toFixed(seconds >= 10 ? 1 : 2)}s`;
  const minutes = Math.floor(seconds / 60);
  const remainder = Math.round(seconds % 60);
  return `${minutes}m ${remainder}s`;
}

function progressBar(ratio) {
  const normalized = Math.max(0, Math.min(1, Number(ratio) || 0));
  const width = 12;
  const filled = Math.round(normalized * width);
  return `${"█".repeat(filled)}${"░".repeat(width - filled)} ${Math.round(normalized * 100)}%`;
}

function phaseLabel(phase) {
  return ({
    recording: "● Recording to SD",
    uploading: "↑ Uploading SD → VPS",
    processing: "◆ Encoding MP4",
    sending: "→ Sending to Telegram",
    reconnecting: "↻ Waiting for Newo2",
    complete: "✓ Delivered",
    failed: "× Failed",
  })[phase] || String(phase || "working");
}

function resolutionLabel(value) {
  const raw = String(value || "").trim().toLowerCase();
  const labels = { qvga: "QVGA · 320×240", vga: "VGA · 640×480", svga: "SVGA · 800×600", xga: "XGA · 1024×768" };
  return labels[raw] || (raw ? raw.toUpperCase() : "n/a");
}

function recordingText(recording) {
  const phase = recording.phase || "recording";
  const elapsedMs = Math.max(0, Date.now() - Number(recording.startedAtMs || Date.now()));
  const targetDurationMs = Number(recording.durationSeconds || 0) * 1000;
  const lines = [`Status: <b>${escapeHtml(phaseLabel(phase))}</b>`];

  if (phase === "recording") {
    if (targetDurationMs > 0) {
      lines.push(`Progress: <code>${progressBar(elapsedMs / targetDurationMs)}</code>`);
      lines.push(`Elapsed: <b>${escapeHtml(formatDuration(Math.min(elapsedMs, targetDurationMs)))}</b> / ${escapeHtml(formatDuration(targetDurationMs))}`);
    } else {
      lines.push(`Elapsed: <b>${escapeHtml(formatDuration(elapsedMs))}</b> · manual stop`);
    }
  } else if (phase === "uploading") {
    const total = Number(recording.expectedBytes || 0);
    const uploaded = Number(recording.uploadedBytes || 0);
    if (total > 0) lines.push(`Progress: <code>${progressBar(uploaded / total)}</code>`);
    lines.push(`Transferred: <b>${escapeHtml(formatBytes(uploaded))}</b>${total > 0 ? ` / ${escapeHtml(formatBytes(total))}` : ""}`);
  } else if (phase === "processing") {
    lines.push("SD capture is safe · building H.264/MP4");
  } else if (phase === "sending") {
    lines.push("MP4 ready · Telegram upload in progress");
  } else if (phase === "reconnecting") {
    lines.push("SD copy is safe · upload resumes after reconnect");
  }

  lines.push(`Video: <b>${escapeHtml(resolutionLabel(recording.resolution))}</b> · <b>${escapeHtml(recording.fps || 20)} FPS</b>${Number.isFinite(Number(recording.quality)) ? ` · JPEG Q${escapeHtml(recording.quality)}` : ""}`);
  return `<b><i>Newo2 recording</i></b>\n<blockquote>${lines.join("\n")}</blockquote>`;
}

export function formatRecordingCaption(recording, message = {}) {
  const durationMs = Number(message.duration_ms || recording.durationMs || 0);
  const frames = Number(message.frames || 0);
  const dropped = Number(message.dropped || 0);
  const fps = Number(recording.fps || message.fps || 20);
  const captureFps = durationMs > 0 && frames > 0 ? frames / (durationMs / 1000) : 0;
  const size = Number(recording.uploadedBytes || message.bytes || 0);
  const resolution = resolutionLabel(recording.resolution || message.resolution);
  const quality = Number(recording.quality ?? message.quality);
  const reason = String(message.reason || "duration_complete");
  const ended = reason === "camera_off" ? "camera gate OFF" : reason === "stopped" ? "manual stop" : "duration complete";

  return [
    "<b>Newo2 · Recording</b>",
    `<b>${escapeHtml(resolution)}</b> · ${escapeHtml(fps)} FPS${Number.isFinite(quality) ? ` · JPEG Q${escapeHtml(quality)}` : ""}`,
    `Duration: <b>${escapeHtml(formatDuration(durationMs))}</b> · Frames: <b>${escapeHtml(frames)}</b> · Dropped: <b>${escapeHtml(dropped)}</b>`,
    `Capture rate: <b>${escapeHtml(captureFps ? captureFps.toFixed(2) : "n/a")} FPS</b> · Size: <b>${escapeHtml(formatBytes(size))}</b>`,
    `Pipeline: <code>SD → VPS → MP4 → Telegram</code> · ${escapeHtml(ended)}`,
  ].join("\n");
}

export function createRecordingUi({ token, logger, updateIntervalMs = 2000 } = {}) {
  async function telegram(method, payload) {
    if (!token) return null;
    try {
      const response = await fetch(`https://api.telegram.org/bot${token}/${method}`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(payload),
        signal: AbortSignal.timeout(12_000),
      });
      const body = await response.json().catch(() => ({}));
      if (!response.ok || body?.ok === false) {
        logger?.warn?.({ method, status: response.status, description: body?.description }, "Newo2 recording UI Telegram call failed");
        return null;
      }
      return body?.result ?? null;
    } catch (error) {
      logger?.warn?.({ method, error: error?.message }, "Newo2 recording UI Telegram call failed");
      return null;
    }
  }

  function stopTimer(recording) {
    if (recording?.uiTimer) clearInterval(recording.uiTimer);
    if (recording) recording.uiTimer = null;
  }

  async function update(recording, force = false) {
    if (!recording?.chatId) return false;
    const text = recordingText(recording);
    if (!force && text === recording.uiLastText) return true;
    recording.uiLastText = text;
    if (!recording.uiMessageId) return false;
    const result = await telegram("editMessageText", {
      chat_id: recording.chatId,
      message_id: recording.uiMessageId,
      text,
      parse_mode: "HTML",
      disable_web_page_preview: true,
    });
    return Boolean(result);
  }

  async function start(recording) {
    if (!recording?.chatId || !token) return false;
    recording.phase = "recording";
    recording.startedAtMs ||= Date.now();
    const text = recordingText(recording);
    const result = await telegram("sendMessage", {
      chat_id: recording.chatId,
      text,
      parse_mode: "HTML",
      disable_web_page_preview: true,
    });
    if (!result?.message_id) return false;
    recording.uiMessageId = result.message_id;
    recording.uiLastText = text;
    stopTimer(recording);
    recording.uiTimer = setInterval(() => { void update(recording); }, Math.max(1500, updateIntervalMs));
    recording.uiTimer.unref?.();
    return true;
  }

  async function phase(recording, nextPhase, fields = {}) {
    if (!recording) return false;
    Object.assign(recording, fields);
    recording.phase = nextPhase;
    return update(recording, true);
  }

  async function complete(recording, message) {
    if (!recording) return false;
    stopTimer(recording);
    recording.phase = "complete";
    const caption = formatRecordingCaption(recording, message);
    const text = `<b><i>Newo2 recording</i></b>\n<blockquote>Status: <b>✓ Delivered</b>\n${caption.replace("<b>Newo2 · Recording</b>\n", "")}</blockquote>`;
    recording.uiLastText = text;
    if (!recording.uiMessageId) return false;
    const result = await telegram("editMessageText", {
      chat_id: recording.chatId,
      message_id: recording.uiMessageId,
      text,
      parse_mode: "HTML",
      disable_web_page_preview: true,
    });
    return Boolean(result);
  }

  async function fail(recording, reason) {
    if (!recording) return false;
    stopTimer(recording);
    recording.phase = "failed";
    const text = `<b><i>Newo2 recording</i></b>\n<blockquote>Status: <b>× Failed</b>\nReason: ${escapeHtml(reason || "unknown")}\nSD copy: <b>${recording.completion?.success ? "preserved" : "check device"}</b></blockquote>`;
    recording.uiLastText = text;
    if (!recording.uiMessageId) return false;
    const result = await telegram("editMessageText", {
      chat_id: recording.chatId,
      message_id: recording.uiMessageId,
      text,
      parse_mode: "HTML",
      disable_web_page_preview: true,
    });
    return Boolean(result);
  }

  return { start, update, phase, complete, fail, stop: stopTimer };
}
