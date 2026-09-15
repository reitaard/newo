const terminal = document.querySelector("#terminal");
const status = document.querySelector("#status");
const meta = document.querySelector("#meta");
const metrics = document.querySelector("#metrics");
const autoscroll = document.querySelector("#autoscroll");
const copy = document.querySelector("#copy");
const decoder = new TextDecoder();
const monitorBase = location.pathname.startsWith("/smonitor2") ? "/smonitor2" : "/smonitor1";
const frameMagic = monitorBase === "/smonitor2" ? "NSM2" : "NSM1";
let receivedBytes = 0;
let lastSequence = 0;
let rawLog = "";
let currentLine = null;

function lineClass(text) {
  const lower = text.toLowerCase();
  if (/error|failed|failure|unsupported|dropped|frame gap/.test(lower)) return "error";
  if (/warn|retry|timeout|connecting/.test(lower)) return "warning";
  if (lower.startsWith("[eyes")) return "eyes";
  if (lower.startsWith("[audio")) return "audio";
  if (lower.startsWith("[cloud")) return "cloud";
  if (lower.startsWith("[remote")) return "remote";
  return "";
}

function appendStyled(parent, text) {
  const tokens = /(\[[^\]\r\n]+\])|(\b[A-Za-z_][\w-]*=)([^\s]+)/g;
  let cursor = 0;
  let match;
  while ((match = tokens.exec(text)) !== null) {
    parent.append(document.createTextNode(text.slice(cursor, match.index)));
    if (match[1]) {
      parent.append(document.createTextNode(match[1]));
    } else {
      const key = document.createElement("span");
      key.className = "log-key";
      key.textContent = match[2];
      const value = document.createElement("span");
      value.className = "log-value";
      value.textContent = match[3];
      parent.append(key, value);
    }
    cursor = match.index + match[0].length;
  }
  parent.append(document.createTextNode(text.slice(cursor)));
}

function renderText(text) {
  for (const part of text.split(/(\n)/)) {
    if (!part) continue;
    if (!currentLine) {
      currentLine = document.createElement("span");
      currentLine.className = "log-line";
      terminal.append(currentLine);
    }
    if (part === "\n") {
      currentLine.append(document.createTextNode(part));
      currentLine = null;
      continue;
    }
    appendStyled(currentLine, part);
    currentLine.className = `log-line ${lineClass(currentLine.textContent)}`.trim();
  }
}

function append(text, diagnostic = false) {
  const output = diagnostic ? `\n[remote] ${text}\n` : text;
  rawLog += output;
  renderText(output);
  if (rawLog.length > 1_000_000) {
    rawLog = rawLog.slice(-750_000);
    terminal.replaceChildren();
    currentLine = null;
    renderText(rawLog);
  }
  if (autoscroll.getAttribute("aria-pressed") === "true") terminal.scrollTop = terminal.scrollHeight;
}

function setStatus(state) {
  const connected = state === "streaming";
  status.textContent = state.split("_").join(" ");
  status.className = `status ${connected ? "connected" : "disconnected"}`;
}

function connect() {
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(`${protocol}//${location.host}${monitorBase}/ws`);
  socket.binaryType = "arraybuffer";
  socket.addEventListener("open", () => setStatus("connecting"));
  socket.addEventListener("message", (event) => {
    if (typeof event.data === "string") {
      const message = JSON.parse(event.data);
      setStatus(message.state ?? "connected");
      const device = message.device;
      if (device) meta.textContent = `${device.id ?? "Newo"} · firmware ${device.firmware ?? "unknown"} · ${device.connected ? "device online" : "device offline"}`;
      if (message.capacity_bytes) meta.textContent += ` · ${(message.capacity_bytes / 1024).toFixed(0)} KiB buffer`;
      return;
    }
    const frame = new Uint8Array(event.data);
    if (frame.length < 12 || String.fromCharCode(...frame.subarray(0, 4)) !== frameMagic) return;
    const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    const sequence = view.getUint32(4, true);
    const dropped = view.getUint32(8, true);
    if (lastSequence && sequence !== lastSequence + 1) append(`network frame gap: expected ${lastSequence + 1}, received ${sequence}`, true);
    if (dropped) append(`${dropped} console bytes dropped before delivery`, true);
    lastSequence = sequence;
    const payload = frame.subarray(12);
    receivedBytes += payload.length;
    metrics.textContent = `${receivedBytes.toLocaleString()} bytes · frame ${sequence}`;
    append(decoder.decode(payload, { stream: true }));
  });
  socket.addEventListener("close", () => { setStatus("reconnecting"); setTimeout(connect, 1500); });
  socket.addEventListener("error", () => socket.close());
}

autoscroll.addEventListener("click", () => {
  const enabled = autoscroll.getAttribute("aria-pressed") !== "true";
  autoscroll.setAttribute("aria-pressed", String(enabled));
  autoscroll.classList.toggle("active", enabled);
  autoscroll.setAttribute("aria-label", `${enabled ? "Disable" : "Enable"} auto-scroll`);
  autoscroll.title = `Auto-scroll ${enabled ? "on" : "off"}`;
  if (enabled) terminal.scrollTop = terminal.scrollHeight;
});

async function copyOutput() {
  if (navigator.clipboard && navigator.clipboard.writeText) {
    await navigator.clipboard.writeText(rawLog);
    return;
  }
  const fallback = document.createElement("textarea");
  fallback.value = rawLog;
  fallback.setAttribute("readonly", "");
  fallback.style.position = "fixed";
  fallback.style.opacity = "0";
  document.body.append(fallback);
  fallback.select();
  const copied = document.execCommand("copy");
  fallback.remove();
  if (!copied) throw new Error("copy denied");
}

copy.addEventListener("click", async () => {
  try {
    await copyOutput();
    copy.classList.add("feedback");
    copy.title = "Copied";
    setTimeout(() => { copy.classList.remove("feedback"); copy.title = "Copy output"; }, 900);
  } catch {
    append("browser denied clipboard access", true);
  }
});

document.querySelector("#clear").addEventListener("click", () => {
  terminal.textContent = "";
  rawLog = "";
  currentLine = null;
  receivedBytes = 0;
  lastSequence = 0;
  metrics.textContent = "0 bytes";
});
connect();
