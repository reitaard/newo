import assert from "node:assert/strict";
import { createServer } from "node:http";
import test from "node:test";

import {
  analyzePcm16,
  createSpeakerRuntime,
  KokoroTtsBackend,
  speakerAudioFilter,
  speakerCreditBytes,
  speakerCodecFlowState,
  speakerDeliveryState,
  speakerOutstandingBytes,
  splitRealtimeText,
  SPEAKER_AUDIO_FILTER,
  SPEAKER_GAIN_DB,
  SPEAKER_LIMITER,
  SPEAKER_MAX_OUTSTANDING_BYTES,
  SPEAKER_NETWORK_INFLIGHT_LIMIT_BYTES,
  SPEAKER_RECEIVER_BUFFER_TARGET_BYTES,
  SPEAKER_RECEIVER_CAPACITY_BYTES,
  startTelegramAndSpeech,
  telegramHtmlToSpeech,
} from "../src/tts.js";

const tick = () => new Promise((resolve) => setImmediate(resolve));
async function waitFor(predicate, attempts = 200) {
  for (let index = 0; index < attempts; index += 1) {
    if (predicate()) return;
    await tick();
  }
  throw new Error("condition not reached");
}

function fakeSocket({ autoFlow = true } = {}) {
  const listeners = new Map();
  return {
    readyState: 1,
    frames: [],
    closeCount: 0,
    deliveredBytes: 0,
    on(name, callback) { listeners.set(name, callback); },
    send(data, options, callback) {
      this.frames.push(data);
      if (autoFlow && Buffer.isBuffer(data)) {
        this.deliveredBytes += data.length;
        listeners.get("message")?.(Buffer.from(JSON.stringify({
          type: "speaker_flow", playback_id: JSON.parse(String(this.frames.find((frame) => !Buffer.isBuffer(frame)))).playback_id,
          received_bytes: this.deliveredBytes, consumed_bytes: this.deliveredBytes,
          buffered_bytes: 0, capacity_bytes: 24_576,
        })), false);
      }
      (typeof options === "function" ? options : callback)?.();
    },
    emitMessage(value) { listeners.get("message")?.(Buffer.from(JSON.stringify(value)), false); },
    close() { this.closeCount += 1; this.readyState = 3; listeners.get("close")?.(); },
  };
}

function logger() { return { info() {}, warn() {} }; }
function binaryFrames(ws) { return ws.frames.filter(Buffer.isBuffer); }

test("Telegram HTML becomes bounded natural speech", () => {
  assert.equal(
    telegramHtmlToSpeech('<b><i>ping:</i></b>\n<blockquote>Status: <b>Online</b>\nLatency: <b>42</b> <i>ms</i></blockquote>'),
    "Status: Online. Latency: 42 milliseconds.",
  );
  assert.equal(telegramHtmlToSpeech("<b>A &amp; B</b> &lt;ready&gt;"), "A & B <ready>");
  assert.ok(telegramHtmlToSpeech("word ".repeat(100), 80).length <= 81);
});

test("delivery-aware flow separates network flight, ESP buffer, and total outstanding", () => {
  assert.equal(SPEAKER_RECEIVER_CAPACITY_BYTES, 24_576);
  assert.equal(SPEAKER_RECEIVER_BUFFER_TARGET_BYTES, 22_528);
  assert.equal(SPEAKER_NETWORK_INFLIGHT_LIMIT_BYTES, 22_528);
  assert.equal(SPEAKER_MAX_OUTSTANDING_BYTES, 22_528);
  assert.deepEqual(speakerDeliveryState(12_288, 8_192, 2_048, 6_144), {
    networkInFlightBytes: 4_096,
    receiverOutstandingBytes: 10_240,
    committedToReceiverBytes: 10_240,
  });
  assert.equal(speakerCreditBytes(0, 0, 0, 0), 22_528, "initial credit leaves one 2 KiB frame of physical headroom");
  assert.equal(speakerCreditBytes(22_528, 0, 0, 0), 0, "sent callbacks do not count as ESP delivery");
  assert.equal(speakerCreditBytes(22_528, 8_192, 0, 8_192), 0, "delivered plus in-flight PCM must not exceed the committed ceiling");
  assert.equal(speakerCreditBytes(22_528, 22_528, 2_048, 20_480), 2_048, "consumption replenishes exactly one frame of credit");
  assert.equal(speakerOutstandingBytes(18_432, 0), 18_432);
  assert.throws(() => speakerDeliveryState(1_024, 2_048, 0, 0), /invalid speaker delivery counters/);
  const opus = speakerCodecFlowState({ logicalPcmProduced: 3_840, wireBytes: 420, opusPackets: 2,
    decodedPcmAdmitted: 3_840, decodedPcmReceived: 1_920, decodedPcmConsumed: 960, decodedPcmBuffered: 960 });
  assert.equal(opus.networkDecodedPcmInFlight, 1_920);
  assert.equal(opus.receiverDecodedPcmOutstanding, 2_880);
  assert.throws(() => speakerCodecFlowState({ logicalPcmProduced: 1, wireBytes: 0, opusPackets: 0, decodedPcmAdmitted: 2, decodedPcmReceived: 0, decodedPcmConsumed: 0, decodedPcmBuffered: 0 }), /ordering/);
});

test("speaker conditioning adds configurable gain before the limiter", () => {
  assert.equal(SPEAKER_GAIN_DB, 2);
  assert.equal(SPEAKER_LIMITER, 0.95);
  assert.match(SPEAKER_AUDIO_FILTER, /highpass=f=110,volume=2dB:precision=float,alimiter=limit=0\.95/);
  assert.match(SPEAKER_AUDIO_FILTER, /attack=5/);
  assert.match(SPEAKER_AUDIO_FILTER, /level=false/);
  assert.match(speakerAudioFilter(3), /volume=3dB/);
});

test("realtime text segmentation makes the first phrase a natural latency-aware clause", () => {
  assert.deepEqual(splitRealtimeText("Face curious."), ["Face curious."]);
  assert.deepEqual(splitRealtimeText("Hello, I'm Newo. This is a realtime voice latency test."), ["Hello, I'm Newo.", "This is a realtime voice latency test."]);
  assert.deepEqual(splitRealtimeText("One two three four five six seven eight", { firstSegmentTargetChars: 20 }), ["One two three four", "five six seven eight"]);
  assert.ok(splitRealtimeText("Yes. The connection is now working correctly.")[0].length >= 14, "do not emit a tiny acknowledgement");
  const unpunctuated = splitRealtimeText("one ".repeat(80).trim(), { maximumSegmentChars: 40, firstSegmentTargetChars: 20 });
  assert.ok(unpunctuated.every((segment) => segment.length <= 40), "all requests remain bounded");
  const long = splitRealtimeText("Newo is online and connected to the cloud. Speaker output is enabled and healthy. The remaining status follows shortly.");
  assert.ok(long.length >= 2);
  assert.equal(long.join(" "), "Newo is online and connected to the cloud. Speaker output is enabled and healthy. The remaining status follows shortly.");
  assert.throws(() => splitRealtimeText("test", { firstSegmentTargetChars: 1 }), /invalid realtime segmentation policy/);
});

test("Kokoro backend uses realtime PCM with configurable voice and speed", async () => {
  let requestBody;
  let requestUrl;
  const sourcePcm = Buffer.alloc(24_000);
  sourcePcm.writeInt16LE(1_000, 2_000);
  const server = createServer((request, response) => {
    requestUrl = request.url;
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => {
      requestBody = JSON.parse(Buffer.concat(chunks).toString("utf8"));
      response.writeHead(200, { "content-type": "application/octet-stream" });
      response.end(sourcePcm);
    });
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  const backend = new KokoroTtsBackend({ baseUrl: `http://127.0.0.1:${port}`, voice: "am_eric", speed: 1.1 });
  try {
    const source = await backend.stream("Natural speech.", { sampleRate: 24_000, channels: 1, bitsPerSample: 16 });
    const chunks = [];
    for await (const chunk of source.audio) chunks.push(chunk);
    const pcm = Buffer.concat(chunks);
    assert.equal(requestUrl, "/v1/audio/realtime");
    assert.ok(pcm.length > 0);
    assert.equal(pcm.length & 1, 0);
    assert.equal(source.metrics.rawPcmBytes, sourcePcm.length, "outer raw metric must aggregate upstream bytes");
    assert.equal(source.metrics.conditionedPcmBytes, pcm.length);
    assert.ok(source.metrics.producerQueueHighWaterBytes > 0);
    assert.deepEqual(requestBody, { model: "kokoro", input: "Natural speech.", voice: "am_eric", response_format: "pcm", speed: 1.1 });
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

test("outer Kokoro metrics aggregate actual raw bytes across text segments", async () => {
  let requests = 0;
  const rawPerSegment = 24_000;
  const server = createServer((_request, response) => {
    requests += 1;
    response.writeHead(200, { "content-type": "application/octet-stream" });
    response.end(Buffer.alloc(rawPerSegment, requests));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  const backend = new KokoroTtsBackend({ baseUrl: `http://127.0.0.1:${port}` });
  try {
    const text = "Newo is online and connected to the cloud. Speaker output is enabled and healthy. The remaining status follows shortly.";
    const source = await backend.stream(text, { sampleRate: 24_000, channels: 1, bitsPerSample: 16 });
    const chunks = [];
    for await (const chunk of source.audio) chunks.push(chunk);
    assert.ok(requests >= 2);
    assert.equal(source.metrics.rawPcmBytes, requests * rawPerSegment);
    assert.equal(source.metrics.conditionedPcmBytes, Buffer.concat(chunks).length);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

test("Kokoro PCM is consumed incrementally before the HTTP response ends", async () => {
  let finishResponse;
  const firstPcm = Buffer.alloc(24_000, 1);
  const server = createServer((_request, response) => {
    response.writeHead(200, { "content-type": "application/octet-stream" });
    response.write(firstPcm);
    finishResponse = () => response.end(Buffer.alloc(24_000, 2));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  const backend = new KokoroTtsBackend({ baseUrl: `http://127.0.0.1:${port}` });
  try {
    const source = await backend.stream("stream me", { sampleRate: 24_000, channels: 1, bitsPerSample: 16 });
    const iterator = source.audio[Symbol.asyncIterator]();
    const firstOutput = await iterator.next();
    assert.equal(firstOutput.done, false);
    assert.ok(firstOutput.value.length > 0, "conditioner must emit before response end");
    finishResponse();
    while (!(await iterator.next()).done) {};
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

test("Kokoro realtime stall fails with a bounded no-progress timeout", async () => {
  let response;
  const server = createServer((_request, current) => {
    response = current;
    current.writeHead(200, { "content-type": "application/octet-stream" });
    current.write(Buffer.alloc(24_000, 1));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  const backend = new KokoroTtsBackend({ baseUrl: `http://127.0.0.1:${port}`, streamNoProgressMs: 200 });
  try {
    const source = await backend.stream("stall", { sampleRate: 24_000, channels: 1, bitsPerSample: 16 });
    const iterator = source.audio[Symbol.asyncIterator]();
    assert.equal((await iterator.next()).done, false);
    await assert.rejects(async () => {
      while (!(await iterator.next()).done) {}
    }, /kokoro_stream_timeout|aborted/);
  } finally {
    response?.destroy();
    await new Promise((resolve) => server.close(resolve));
  }
});

test("Kokoro iterator cancellation aborts HTTP production and waits for cleanup", async () => {
  let writes = 0;
  let closed = false;
  const server = createServer((_request, response) => {
    response.writeHead(200, { "content-type": "application/octet-stream" });
    const timer = setInterval(() => {
      writes += 1;
      response.write(Buffer.alloc(24_000, writes & 0xff));
    }, 5);
    response.on("close", () => { clearInterval(timer); closed = true; });
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  const backend = new KokoroTtsBackend({ baseUrl: `http://127.0.0.1:${port}` });
  try {
    const source = await backend.stream("cancel", { sampleRate: 24_000, channels: 1, bitsPerSample: 16 });
    const iterator = source.audio[Symbol.asyncIterator]();
    assert.equal((await iterator.next()).done, false);
    await iterator.return();
    await waitFor(() => closed);
    const writesAfterCancel = writes;
    await new Promise((resolve) => setTimeout(resolve, 30));
    assert.equal(writes, writesAfterCancel, "HTTP producer must stop after iterator cancellation");
    assert.equal(source.metrics.producerQueuedBytes, 0);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

test("Telegram send starts before independent TTS and is not blocked by it", async () => {
  const order = [];
  let finishTelegram;
  const telegram = new Promise((resolve) => { finishTelegram = resolve; });
  const result = startTelegramAndSpeech(
    () => { order.push("telegram"); return telegram; },
    () => { order.push("tts"); },
  );
  assert.deepEqual(order, ["telegram", "tts"]);
  assert.equal(result, telegram);
  finishTelegram("sent");
  assert.equal(await result, "sent");
});

test("Kokoro backend reports an unavailable local service clearly", async () => {
  const backend = new KokoroTtsBackend({ baseUrl: "http://127.0.0.1:1", requestTimeoutMs: 1_000 });
  await assert.rejects(
    backend.synthesize("test", { sampleRate: 24_000, channels: 1, bitsPerSample: 16 }),
    /Kokoro unavailable/,
  );
});

test("Kokoro backend rejects compressed or malformed output", async () => {
  for (const fixture of [
    { type: "audio/mpeg", body: Buffer.from("ID3") },
    { type: "application/octet-stream", body: Buffer.from([1]) },
  ]) {
    const server = createServer((request, response) => { response.writeHead(200, { "content-type": fixture.type }); response.end(fixture.body); });
    await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
    const { port } = server.address();
    const backend = new KokoroTtsBackend({ baseUrl: `http://127.0.0.1:${port}`, conditioner: async (pcm) => pcm });
    try {
      await assert.rejects(
        backend.synthesize("test", { sampleRate: 24_000, channels: 1, bitsPerSample: 16 }),
        /non-PCM content type|invalid_pcm|invalid PCM16/,
      );
    } finally {
      await new Promise((resolve) => server.close(resolve));
    }
  }
});

test("final PCM diagnostics report peak, RMS, limiter reach, and clipping", () => {
  const pcm = Buffer.alloc(8);
  pcm.writeInt16LE(0, 0);
  pcm.writeInt16LE(16_384, 2);
  pcm.writeInt16LE(-16_384, 4);
  pcm.writeInt16LE(0, 6);
  const diagnostics = analyzePcm16(pcm, 0.95);
  assert.equal(diagnostics.peakDbfs.toFixed(1), "-6.0");
  assert.equal(diagnostics.rmsDbfs.toFixed(1), "-9.0");
  assert.equal(diagnostics.limiterReached, false);
  assert.equal(diagnostics.clipped, false);
  assert.equal(diagnostics.bytes, 8);
});

test("speaker runtime bounds queued jobs", () => {
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { synthesize: () => new Promise(() => {}), gainDb: 6, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true, maxPendingJobs: 2,
  });
  assert.equal(runtime.speak("one").kind, "queued");
  assert.equal(runtime.speak("two").kind, "queued");
  assert.equal(runtime.speak("three").kind, "busy");
  runtime.close();
});

test("streaming speaker framing emits PCM before synthesis ends", async () => {
  let releaseTail;
  const ws = fakeSocket();
  const backend = {
    gainDb: 2, limiter: 0.95,
    async stream() {
      const metrics = { requestStartedAt: performance.now(), firstAudioByteAt: performance.now(), conditionerFirstOutputAt: performance.now(), completedAt: null };
      return { metrics, audio: (async function* () {
        yield Buffer.alloc(8_192, 1);
        await new Promise((resolve) => { releaseTail = resolve; });
        yield Buffer.alloc(4_096, 2);
        metrics.completedAt = performance.now();
      })() };
    },
  };
  const runtime = createSpeakerRuntime({ logger: logger(), enabled: true, backend, getDevice: () => ({}), sendControl: () => true });
  runtime.handleConnection(ws, "newo-01");
  const queued = runtime.speak("streaming");
  await waitFor(() => binaryFrames(ws).length > 0);
  const begin = JSON.parse(String(ws.frames[0]));
  assert.equal(begin.streaming, true);
  assert.equal(begin.max_bytes, 2_880_000);
  assert.equal(ws.frames.some((frame) => String(frame).includes("speaker_end")), false);
  releaseTail();
  await waitFor(() => ws.frames.some((frame) => String(frame).includes("speaker_end")));
  runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: queued.playbackId, bytes: 12_288 });
  await queued.completion;
  runtime.close();
});

test("speaker failure cancels an active source before the next queued job", async () => {
  const ws = fakeSocket({ autoFlow: false });
  let streamCalls = 0;
  let produced = 0;
  let cancelled = false;
  const backend = {
    gainDb: 2, limiter: 0.95,
    async stream() {
      streamCalls += 1;
      const metrics = {};
      if (streamCalls > 1) return { metrics, audio: (async function* () { yield Buffer.alloc(4_096, 2); })() };
      return { metrics, audio: (async function* () {
        const producer = setInterval(() => { produced += 1; }, 1);
        try {
          while (true) {
            produced += 1;
            yield Buffer.alloc(2_048, 1);
            await tick();
          }
        } finally {
          clearInterval(producer);
          cancelled = true;
        }
      })() };
    },
  };
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true, backend, getDevice: () => ({}), sendControl: () => true,
    flowTimeoutMs: 25,
  });
  runtime.handleConnection(ws, "newo-01");
  const failed = runtime.speak("cancel this stream");
  await assert.rejects(failed.completion, /flow_timeout/);
  assert.equal(cancelled, true, "source finally must run before failed playback settles");
  const producedAtCancel = produced;
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(produced, producedAtCancel, "cancelled producer must not emit more PCM");

  const next = runtime.speak("next stream");
  await waitFor(() => ws.frames.filter((frame) => String(frame).includes("speaker_end")).length === 1);
  assert.equal(streamCalls, 2, "next job must not remain blocked behind the cancelled producer");
  runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: next.playbackId, bytes: 4_096 });
  await next.completion;
  runtime.close();
});

test("streaming speaker enforces the cumulative byte maximum", async () => {
  const ws = fakeSocket();
  const backend = { gainDb: 2, limiter: 0.95, async stream() {
    return { metrics: {}, audio: (async function* () { yield Buffer.alloc(6_000, 1); })() };
  } };
  const runtime = createSpeakerRuntime({ logger: logger(), enabled: true, backend, getDevice: () => ({}), sendControl: () => true, maxStreamBytes: 4_096 });
  runtime.handleConnection(ws, "newo-01");
  const queued = runtime.speak("too large");
  await assert.rejects(queued.completion, /exceeded limit/);
  runtime.close();
});

test("delivery credit waits for actual ESP receipt and replenishes its reported buffer", async () => {
  const ws = fakeSocket({ autoFlow: false });
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(24_576, 1); }, gainDb: 2, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true,
    flowTimeoutMs: 500,
  });
  runtime.handleConnection(ws, "newo-01");
  const queued = runtime.speak("flow controlled");

  await waitFor(() => binaryFrames(ws).length === 11);
  await tick();
  assert.equal(binaryFrames(ws).length, 11, "sender must stop at the 22.5 KiB committed window before receipt");
  assert.equal(ws.frames.some((frame) => String(frame).includes("speaker_end")), false);

  let received = 0;
  let consumed = 0;
  let buffered = 0;
  for (let step = 0; step < 30 && !ws.frames.some((frame) => String(frame).includes("speaker_end")); step += 1) {
    const sent = binaryFrames(ws).reduce((sum, frame) => sum + frame.length, 0);
    if (received < sent) { buffered += sent - received; received = sent; }
    else if (buffered > 0) { const amount = Math.min(2_048, buffered); buffered -= amount; consumed += amount; }
    ws.emitMessage({
      type: "speaker_flow", playback_id: queued.playbackId, received_bytes: received,
      consumed_bytes: consumed, buffered_bytes: buffered, capacity_bytes: 24_576,
    });
    await tick();
  }

  await waitFor(() => ws.frames.some((frame) => String(frame).includes("speaker_end")));
  assert.equal(binaryFrames(ws).length, 12);
  runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: queued.playbackId, bytes: 24_576 });
  assert.equal((await queued.completion).kind, "complete");
  runtime.close();
});

test("delivery-aware sliding window survives delayed jittered receiver delivery", async () => {
  const totalBytes = 144_000;
  const forwardDelays = [15, 2, 8, 4, 12, 3, 6, 10, 5, 14, 2, 7]; // 20-150 ms at 10x simulation speed.
  const reverseDelays = [2, 5, 1, 3, 4];
  const listeners = new Map();
  const network = [];
  const acknowledgements = [];
  let tickNumber = 0;
  let forwardIndex = 0;
  let reverseIndex = 0;
  let lastNetworkDue = 0;
  let lastAckDue = 0;
  let playbackId = null;
  let endBytes = null;
  let endDelivered = false;
  let received = 0;
  let consumed = 0;
  let buffered = 0;
  let playbackStarted = false;
  let lastReportedReceived = 0;
  let lastReportedConsumed = 0;
  let lastFlowTick = 0;
  let maxBuffer = 0;
  let minActiveBuffer = Number.POSITIVE_INFINITY;
  let maxNetworkInFlight = 0;
  let maxQueuedFrames = 0;
  let overflows = 0;
  let underruns = 0;
  const underrunEvents = [];

  const ws = {
    readyState: 1,
    frames: [],
    on(name, callback) { listeners.set(name, callback); },
    close() { this.readyState = 3; listeners.get("close")?.(); },
    send(data, options, callback) {
      this.frames.push(data);
      if (!Buffer.isBuffer(data)) {
        const message = JSON.parse(String(data));
        if (message.type === "speaker_begin") playbackId = message.playback_id;
        if (message.type === "speaker_end") {
          endBytes = message.bytes;
          // speaker_end follows the final PCM frame on the same ordered TCP stream;
          // it does not pay an independent full-path jitter delay.
          lastNetworkDue = Math.max(lastNetworkDue, tickNumber + 1);
          network.push({ due: lastNetworkDue, end: true });
        }
      } else {
        let delay = forwardDelays[forwardIndex++ % forwardDelays.length];
        if (forwardIndex % 11 === 0) delay += 3; // 30 ms receiver scheduling stall.
        lastNetworkDue = Math.max(lastNetworkDue, tickNumber + delay);
        network.push({ due: lastNetworkDue, pcm: data });
        maxQueuedFrames = Math.max(maxQueuedFrames, network.filter((item) => item.pcm).length);
      }
      (typeof options === "function" ? options : callback)?.(); // Accepted immediately, not delivered.
    },
  };

  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(totalBytes, 1); }, gainDb: 2, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true, flowTimeoutMs: 1_000,
  });
  runtime.handleConnection(ws, "newo-01");
  const queued = runtime.speak("jitter simulation");

  function scheduleFlow() {
    const snapshot = {
      type: "speaker_flow", playback_id: playbackId, received_bytes: received,
      consumed_bytes: consumed, buffered_bytes: buffered, capacity_bytes: 24_576,
    };
    lastAckDue = Math.max(lastAckDue, tickNumber + reverseDelays[reverseIndex++ % reverseDelays.length]);
    acknowledgements.push({ due: lastAckDue, snapshot });
    lastReportedReceived = received;
    lastReportedConsumed = consumed;
    lastFlowTick = tickNumber;
  }

  for (; tickNumber < 2_000; tickNumber += 1) {
    for (const item of network.splice(0, network.filter((entry) => entry.due <= tickNumber).length)) {
      if (item.end) { endDelivered = true; continue; }
      if (buffered + item.pcm.length > 24_576) { overflows += 1; continue; }
      received += item.pcm.length;
      buffered += item.pcm.length;
      maxBuffer = Math.max(maxBuffer, buffered);
      if (received - lastReportedReceived >= 2_048) scheduleFlow();
    }
    if (!playbackStarted && buffered >= 12_288) playbackStarted = true;
    if (playbackStarted) {
      if (buffered > 0) {
        const amount = Math.min(480, buffered); // 48,000 bytes/s, 10 ms per simulated tick.
        buffered -= amount;
        consumed += amount;
        if (!endDelivered || consumed < received) minActiveBuffer = Math.min(minActiveBuffer, buffered);
      } else if (!endDelivered || consumed < received) {
        underruns += 1;
        underrunEvents.push({ tickNumber, received, consumed, pending: network.length, acks: acknowledgements.length });
      }
      if (consumed - lastReportedConsumed >= 1_024 ||
          (buffered < 10_240 && tickNumber - lastFlowTick >= 4)) scheduleFlow();
    }
    const dueAcks = acknowledgements.filter((entry) => entry.due <= tickNumber);
    acknowledgements.splice(0, dueAcks.length);
    for (const ack of dueAcks) listeners.get("message")?.(Buffer.from(JSON.stringify(ack.snapshot)), false);
    const sent = ws.frames.filter(Buffer.isBuffer).reduce((sum, frame) => sum + frame.length, 0);
    maxNetworkInFlight = Math.max(maxNetworkInFlight, sent - received);
    if (endDelivered && endBytes === totalBytes && received === totalBytes && consumed === totalBytes) {
      runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: queued.playbackId, bytes: totalBytes });
      break;
    }
    await tick();
  }

  assert.equal((await queued.completion).kind, "complete");
  assert.equal(received, totalBytes);
  assert.equal(consumed, totalBytes);
  assert.equal(endBytes, totalBytes);
  assert.equal(overflows, 0);
  assert.equal(underruns, 0, `min=${minActiveBuffer} max=${maxBuffer} inflight=${maxNetworkInFlight} queued=${maxQueuedFrames} events=${JSON.stringify(underrunEvents)}`);
  assert.ok(minActiveBuffer > 0, `receiver starved: min=${minActiveBuffer}`);
  assert.ok(maxBuffer <= 24_576);
  assert.ok(maxNetworkInFlight <= 22_528, `network flight exceeded: ${maxNetworkInFlight}`);
  assert.ok(maxQueuedFrames >= 3, "sender regressed to stop-and-wait delivery");
  runtime.close();
});

test("speaker runtime fails rather than guessing when receiver flow stops", async () => {
  const ws = fakeSocket({ autoFlow: false });
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(24_576, 1); }, gainDb: 2, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true,
    flowTimeoutMs: 20,
  });
  runtime.handleConnection(ws, "newo-01");
  const queued = runtime.speak("no receiver credit");
  await assert.rejects(queued.completion, /flow_timeout/);
  assert.equal(binaryFrames(ws).length, 11);
  runtime.close();
});

test("one persistent speaker connection is reused for 10 framed playbacks", async () => {
  const ws = fakeSocket();
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(4_096, 1); }, gainDb: 6, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true,
  });
  runtime.handleConnection(ws, "newo-01");

  const first = runtime.speak("first");
  await waitFor(() => ws.frames.some((frame) => String(frame).includes("speaker_end")));
  const firstFrames = [...ws.frames];
  assert.match(String(firstFrames[0]), /"type":"speaker_begin"/);
  assert.deepEqual(firstFrames.filter(Buffer.isBuffer).map((frame) => frame.length), [2_048, 2_048]);
  assert.match(String(firstFrames.at(-1)), /"type":"speaker_end"/);
  assert.equal(ws.closeCount, 0);
  runtime.handlePlaybackStarted("newo-01", { playback_id: first.playbackId, first_pcm_to_play_ms: 128 });
  runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: first.playbackId, bytes: 4_096 });
  assert.equal((await first.completion).kind, "complete");

  for (let number = 2; number <= 10; number += 1) {
    const frameCount = ws.frames.length;
    const queued = runtime.speak(`playback ${number}`);
    await waitFor(() => ws.frames.length > frameCount && String(ws.frames.at(-1)).includes("speaker_end"));
    runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: queued.playbackId, bytes: 4_096 });
    await queued.completion;
  }
  assert.equal(ws.closeCount, 0);
  assert.equal(ws.frames.filter((frame) => String(frame).includes("speaker_begin")).length, 10);
  runtime.close();
});

test("speaker completion verifies playback ID and final byte count", async () => {
  const ws = fakeSocket();
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(4_096, 1); }, gainDb: 2, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true,
  });
  runtime.handleConnection(ws, "newo-01");
  const queued = runtime.speak("verify");
  await waitFor(() => ws.frames.some((frame) => String(frame).includes("speaker_end")));
  assert.equal(runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: "00000000-0000-4000-8000-000000000000", bytes: 4_096 }), false);
  runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: queued.playbackId, bytes: 2_048 });
  await assert.rejects(queued.completion, /truncated/);
  runtime.close();
});

test("speaker runtime fails quickly when no persistent stream becomes ready", async () => {
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(512); }, gainDb: 6, limiter: 0.95 },
    getDevice: () => ({}), sendControl: () => true,
    connectionTimeoutMs: 20,
  });
  const queued = runtime.speak("hello");
  const keepAlive = setTimeout(() => {}, 100);
  await assert.rejects(queued.completion, /speaker connection timeout/);
  clearTimeout(keepAlive);
  runtime.close();
});

test("manual speech requests a temporary stream and disconnects it after completion", async () => {
  let persistentEnabled = false;
  const controls = [];
  const runtime = createSpeakerRuntime({
    logger: logger(), enabled: true,
    backend: { async synthesize() { return Buffer.alloc(512); }, gainDb: 6, limiter: 0.95 },
    getDevice: () => ({}),
    sendControl: (message, device) => { controls.push({ message, device }); return true; },
    isPersistentEnabled: () => persistentEnabled,
  });
  const queued = runtime.speak("manual", { temporary: true });
  await waitFor(() => controls.length === 1);
  assert.deepEqual(controls[0].message, { type: "speaker_control", action: "temporary_connect" });
  const ws = fakeSocket();
  runtime.handleConnection(ws, "newo-01");
  await waitFor(() => String(ws.frames.at(-1)).includes("speaker_end"));
  runtime.handleResult("newo-01", { type: "speaker_complete", playback_id: queued.playbackId, bytes: 512 });
  await queued.completion;
  assert.equal(ws.closeCount, 1);
  assert.equal(persistentEnabled, false);
  runtime.close();
});
