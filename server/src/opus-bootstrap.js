// DEPRECATED prototype / rollback only. Native codec-aware transport now lives
// in opus-transport.js and normal production should use SPEAKER_CODEC=opus with
// NODE_OPTIONS="". Keep this preload only to roll back a native deployment.
// Optional transparent Opus transport for the pre-native speaker runtime.
// Enable only after flashing Opus-capable firmware:
//   SPEAKER_CODEC=opus NODE_OPTIONS="--import ./src/opus-bootstrap.js"
//
// Existing tts.js continues accounting in original PCM bytes. This shim only
// compresses the WebSocket wire representation, then the ESP reports decoded
// PCM progress using the same counters as before.

const enabled = String(process.env.SPEAKER_CODEC ?? "pcm").trim().toLowerCase() === "opus";

if (enabled) {
  const [{ default: WebSocket }, opus] = await Promise.all([
    import("ws"),
    import("libopus-wasm"),
  ]);

  const { createEncoder, Application, Signal } = opus;
  const installed = Symbol.for("newo.opus.transport.installed");

  if (!WebSocket.prototype[installed]) {
    Object.defineProperty(WebSocket.prototype, installed, { value: true });

    const originalSend = WebSocket.prototype.send;
    const originalEmit = WebSocket.prototype.emit;
    const states = new WeakMap();
    const capabilities = new WeakMap();
    const FRAME_MS = 40;
    const SAMPLE_RATE = 24_000;
    const FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1_000;
    const FRAME_PCM_BYTES = FRAME_SAMPLES * 2;
    const BITRATE = 24_000;
    const HEADER_BYTES = 8;
    const MAGIC = Buffer.from("NWOP", "ascii");

    function sendOriginal(ws, data, options) {
      return new Promise((resolve, reject) => {
        const done = (error) => error ? reject(error) : resolve();
        try {
          if (options === undefined) originalSend.call(ws, data, done);
          else originalSend.call(ws, data, options, done);
        } catch (error) {
          reject(error);
        }
      });
    }

    function finishCallback(callback, promise) {
      promise.then(
        () => { if (typeof callback === "function") callback(); },
        (error) => { if (typeof callback === "function") callback(error); },
      );
      promise.catch(() => {});
    }

    function parseJsonText(data) {
      if (typeof data !== "string" || data.length < 2 || data[0] !== "{") return null;
      try { return JSON.parse(data); } catch { return null; }
    }

    class PlaybackEncoder {
      constructor(playbackId) {
        this.playbackId = playbackId;
        this.carry = Buffer.alloc(0);
        this.sequence = 0;
        this.rawPcmBytes = 0;
        this.opusPayloadBytes = 0;
        this.wireBytes = 0;
        this.packetCount = 0;
        this.freed = false;
        this.encoderPromise = createEncoder({
          sampleRate: SAMPLE_RATE,
          channels: 1,
          frameSize: FRAME_SAMPLES,
          application: Application.Voip,
          bitrate: BITRATE,
          signal: Signal.Voice,
          vbr: true,
        }).then((encoder) => {
          if (encoder.frameSize !== FRAME_SAMPLES || encoder.sampleRate !== SAMPLE_RATE || encoder.channels !== 1) {
            encoder.free();
            throw new Error("unexpected Opus encoder format");
          }
          return encoder;
        });
      }

      async encodeFrame(frame, validPcmBytes) {
        if (frame.length !== FRAME_PCM_BYTES || validPcmBytes <= 0 || validPcmBytes > FRAME_PCM_BYTES || (validPcmBytes & 1)) {
          throw new Error("invalid Opus PCM frame");
        }
        const encoder = await this.encoderPromise;
        const packet = encoder.encode(new Uint8Array(frame.buffer, frame.byteOffset, frame.byteLength));
        if (!packet?.byteLength || packet.byteLength > 4_000 - HEADER_BYTES) throw new Error("invalid Opus packet");
        if (this.sequence > 0xffff) throw new Error("Opus sequence overflow");

        const envelope = Buffer.allocUnsafe(HEADER_BYTES + packet.byteLength);
        MAGIC.copy(envelope, 0);
        envelope.writeUInt16LE(this.sequence, 4);
        envelope.writeUInt16LE(validPcmBytes, 6);
        Buffer.from(packet).copy(envelope, HEADER_BYTES);
        this.sequence += 1;
        this.packetCount += 1;
        this.opusPayloadBytes += packet.byteLength;
        this.wireBytes += envelope.length;
        return envelope;
      }

      async push(pcm) {
        if (!Buffer.isBuffer(pcm) || !pcm.length || (pcm.length & 1)) throw new Error("invalid PCM for Opus transport");
        this.rawPcmBytes += pcm.length;
        this.carry = this.carry.length ? Buffer.concat([this.carry, pcm]) : Buffer.from(pcm);
        const packets = [];
        while (this.carry.length >= FRAME_PCM_BYTES) {
          const frame = this.carry.subarray(0, FRAME_PCM_BYTES);
          this.carry = this.carry.subarray(FRAME_PCM_BYTES);
          packets.push(await this.encodeFrame(frame, FRAME_PCM_BYTES));
        }
        return packets;
      }

      async finish() {
        const packets = [];
        if (this.carry.length) {
          const validPcmBytes = this.carry.length;
          const padded = Buffer.alloc(FRAME_PCM_BYTES);
          this.carry.copy(padded);
          this.carry = Buffer.alloc(0);
          packets.push(await this.encodeFrame(padded, validPcmBytes));
        }
        await this.free();
        return packets;
      }

      async free() {
        if (this.freed) return;
        this.freed = true;
        try {
          const encoder = await this.encoderPromise;
          encoder.free();
        } catch {
          // Encoder construction failures are surfaced by the active send path.
        }
      }
    }

    WebSocket.prototype.emit = function newoOpusEmit(event, ...args) {
      if (event === "message" && args.length) {
        try {
          const data = args[0];
          const isBinary = Boolean(args[1]);
          if (!isBinary) {
            const text = Buffer.isBuffer(data) ? data.toString("utf8") : String(data);
            const message = text.length > 1 && text[0] === "{" ? JSON.parse(text) : null;
            if (message?.type === "speaker_ready") {
              const codecs = Array.isArray(message.codecs) ? new Set(message.codecs.map((value) => String(value).toLowerCase())) : new Set();
              capabilities.set(this, codecs);
              if (codecs.has("opus")) console.log("[newo-opus] speaker advertised Opus support");
            }
          }
        } catch {
          // Capability detection is advisory; normal websocket handling continues.
        }
      }
      return originalEmit.call(this, event, ...args);
    };

    WebSocket.prototype.send = function newoOpusSend(data, options, callback) {
      let sendOptions = options;
      let sendCallback = callback;
      if (typeof options === "function") {
        sendCallback = options;
        sendOptions = undefined;
      }

      const message = parseJsonText(data);
      if (message?.type === "speaker_begin") {
        if (!capabilities.get(this)?.has("opus")) return originalSend.call(this, data, options, callback);
        const existing = states.get(this);
        if (existing) void existing.encoder.free();
        const state = {
          encoder: new PlaybackEncoder(message.playback_id),
          chain: Promise.resolve(),
        };
        states.set(this, state);
        this.once?.("close", () => { void state.encoder.free(); states.delete(this); });
        message.codec = "opus";
        message.opus_bitrate = BITRATE;
        message.opus_frame_ms = FRAME_MS;
        message.opus_frame_pcm_bytes = FRAME_PCM_BYTES;
        if (sendOptions === undefined) return originalSend.call(this, JSON.stringify(message), sendCallback);
        return originalSend.call(this, JSON.stringify(message), sendOptions, sendCallback);
      }

      const state = states.get(this);
      if (!state) return originalSend.call(this, data, options, callback);

      const isPcmBinary = Buffer.isBuffer(data) || data instanceof Uint8Array;
      if (isPcmBinary) {
        const pcm = Buffer.from(data);
        const operation = state.chain.then(async () => {
          const packets = await state.encoder.push(pcm);
          for (const packet of packets) await sendOriginal(this, packet, { binary: true, compress: false });
        });
        state.chain = operation;
        finishCallback(sendCallback, operation);
        return;
      }

      if (message?.type === "speaker_end") {
        const operation = state.chain.then(async () => {
          const packets = await state.encoder.finish();
          for (const packet of packets) await sendOriginal(this, packet, { binary: true, compress: false });
          await sendOriginal(this, data, sendOptions);
          const stats = state.encoder;
          const ratio = stats.wireBytes > 0 ? stats.rawPcmBytes / stats.wireBytes : null;
          console.log(JSON.stringify({
            event: "SPEAKER_OPUS_WIRE",
            playback_id: stats.playbackId,
            raw_pcm_bytes: stats.rawPcmBytes,
            opus_payload_bytes: stats.opusPayloadBytes,
            wire_bytes: stats.wireBytes,
            packets: stats.packetCount,
            frame_ms: FRAME_MS,
            bitrate: BITRATE,
            compression_ratio: ratio == null ? null : Number(ratio.toFixed(2)),
          }));
          states.delete(this);
        });
        state.chain = operation;
        finishCallback(sendCallback, operation);
        return;
      }

      return originalSend.call(this, data, options, callback);
    };

    console.log(`[newo-opus] enabled: ${SAMPLE_RATE} Hz mono, ${BITRATE} bps, ${FRAME_MS} ms frames`);
  }
}
