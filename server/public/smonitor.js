const terminal = document.querySelector("#terminal");
const status = document.querySelector("#status");
const meta = document.querySelector("#meta");
const metrics = document.querySelector("#metrics");
const autoscroll = document.querySelector("#autoscroll");
const decoder = new TextDecoder();
let receivedBytes = 0;
let lastSequence = 0;

function append(text, diagnostic = false) {
  terminal.append(document.createTextNode(diagnostic ? `\n[remote] ${text}\n` : text));
  if (terminal.textContent.length > 1_000_000) terminal.textContent = terminal.textContent.slice(-750_000);
  if (autoscroll.checked) terminal.scrollTop = terminal.scrollHeight;
}

function setStatus(state) {
  const connected = state === "streaming";
  status.textContent = state.replaceAll("_", " ");
  status.className = `status ${connected ? "connected" : "disconnected"}`;
}

function connect() {
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(`${protocol}//${location.host}/smonitor/ws`);
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
    if (frame.length < 12 || String.fromCharCode(...frame.subarray(0, 4)) !== "NSM1") return;
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

document.querySelector("#clear").addEventListener("click", () => { terminal.textContent = ""; receivedBytes = 0; metrics.textContent = "0 bytes"; });
connect();
