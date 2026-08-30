import { createEncoder, Application, Signal } from "libopus-wasm";

export const OPUS_TRANSPORT = Object.freeze({ sampleRate: 24_000, channels: 1, bitrate: 24_000, frameMs: 40, headerBytes: 8, maxPacketBytes: 4_000 });
const frameSamples = OPUS_TRANSPORT.sampleRate * OPUS_TRANSPORT.frameMs / 1000;
export const OPUS_FRAME_PCM_BYTES = frameSamples * 2;
const magic = Buffer.from("NWOP", "ascii");

export class OpusPlaybackTransport {
  constructor({ playbackId, enabled = false } = {}) { this.playbackId = playbackId; this.enabled = enabled; this.carry = Buffer.alloc(0); this.sequence = 0; this.encoder = null; this.closed = false; this.stats = { rawPcmBytes: 0, opusPayloadBytes: 0, wireBytes: 0, packets: 0 }; }
  static supported(codecs) { return codecs instanceof Set && codecs.has("opus"); }
  async begin(send) {
    if (!this.enabled) return;
    this.encoder = await createEncoder({ sampleRate: OPUS_TRANSPORT.sampleRate, channels: 1, frameSize: frameSamples, application: Application.Voip, bitrate: OPUS_TRANSPORT.bitrate, signal: Signal.Voice, vbr: true });
    if (this.encoder.frameSize !== frameSamples || this.encoder.sampleRate !== OPUS_TRANSPORT.sampleRate || this.encoder.channels !== 1) throw new Error("unexpected Opus encoder format");
  }
  beginMessage(message) { return this.enabled ? { ...message, codec: "opus", opus_bitrate: OPUS_TRANSPORT.bitrate, opus_frame_ms: OPUS_TRANSPORT.frameMs, opus_frame_pcm_bytes: OPUS_FRAME_PCM_BYTES } : message; }
  async sendPcm(send, pcm) {
    if (!this.enabled) return send(pcm, { binary: true });
    if (!Buffer.isBuffer(pcm) || !pcm.length || (pcm.length & 1)) throw new Error("invalid_pcm");
    this.stats.rawPcmBytes += pcm.length;
    this.carry = this.carry.length ? Buffer.concat([this.carry, pcm]) : Buffer.from(pcm);
    while (this.carry.length >= OPUS_FRAME_PCM_BYTES) { const frame = this.carry.subarray(0, OPUS_FRAME_PCM_BYTES); this.carry = this.carry.subarray(OPUS_FRAME_PCM_BYTES); await send(await this.encode(frame, OPUS_FRAME_PCM_BYTES), { binary: true, compress: false }); }
  }
  async finish(send) {
    if (!this.enabled) return;
    if (this.carry.length) { const valid = this.carry.length; const frame = Buffer.alloc(OPUS_FRAME_PCM_BYTES); this.carry.copy(frame); this.carry = Buffer.alloc(0); await send(await this.encode(frame, valid), { binary: true, compress: false }); }
    await this.free();
  }
  async encode(frame, validPcmBytes) {
    if (!this.encoder || this.sequence > 0xffff) throw new Error("opus_encoder_unavailable");
    const packet = this.encoder.encode(new Uint8Array(frame.buffer, frame.byteOffset, frame.length));
    if (!packet?.byteLength || packet.byteLength > OPUS_TRANSPORT.maxPacketBytes - OPUS_TRANSPORT.headerBytes) throw new Error("invalid_opus_packet");
    const envelope = Buffer.allocUnsafe(OPUS_TRANSPORT.headerBytes + packet.byteLength);
    magic.copy(envelope); envelope.writeUInt16LE(this.sequence++, 4); envelope.writeUInt16LE(validPcmBytes, 6); Buffer.from(packet).copy(envelope, 8);
    this.stats.packets++; this.stats.opusPayloadBytes += packet.byteLength; this.stats.wireBytes += envelope.length;
    return envelope;
  }
  async free() { if (this.closed) return; this.closed = true; try { this.encoder?.free(); } finally { this.encoder = null; } }
}
