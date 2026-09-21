export const XIAOMEI_SWITCH_START = "__newo_switch_xiaomei_start__";
export const XIAOMEI_SWITCH_READY = "__newo_switch_xiaomei_ready__";
export const ALFRED_SWITCH_START = "__newo_switch_alfred_start__";
export const ALFRED_SWITCH_READY = "__newo_switch_alfred_ready__";

const SAMPLE_RATE = 24_000;
const AMPLITUDE = 0.16;
const EARCONS = new Map([
  [XIAOMEI_SWITCH_START, { first: 620, second: 900, firstMs: 85, gapMs: 28, secondMs: 120 }],
  [XIAOMEI_SWITCH_READY, { first: 760, second: 1_020, firstMs: 55, gapMs: 20, secondMs: 75 }],
  [ALFRED_SWITCH_START, { first: 900, second: 620, firstMs: 85, gapMs: 28, secondMs: 120 }],
  [ALFRED_SWITCH_READY, { first: 820, second: 560, firstMs: 55, gapMs: 20, secondMs: 75 }],
]);
const PHRASE_ALIASES = new Map([
  ["Switching to Xiaomei.", XIAOMEI_SWITCH_START],
  ["Xiaomei ready.", XIAOMEI_SWITCH_READY],
  ["Switching to Alfred.", ALFRED_SWITCH_START],
  ["Alfred ready.", ALFRED_SWITCH_READY],
]);

function canonicalEarcon(text) {
  const input = String(text ?? "");
  return EARCONS.has(input) ? input : PHRASE_ALIASES.get(input) ?? null;
}

export function isSwitchEarcon(text) {
  return canonicalEarcon(text) !== null;
}

function toneSample(frequency, index, total) {
  const edge = Math.max(1, Math.floor(SAMPLE_RATE * 0.008));
  const attack = Math.min(1, index / edge);
  const release = Math.min(1, (total - 1 - index) / edge);
  const envelope = Math.max(0, Math.min(attack, release));
  return Math.round(Math.sin(2 * Math.PI * frequency * index / SAMPLE_RATE) * 32_767 * AMPLITUDE * envelope);
}

export function renderSwitchEarcon(text) {
  const key = canonicalEarcon(text);
  const spec = key ? EARCONS.get(key) : null;
  if (!spec) return null;
  const firstFrames = Math.round(SAMPLE_RATE * spec.firstMs / 1_000);
  const gapFrames = Math.round(SAMPLE_RATE * spec.gapMs / 1_000);
  const secondFrames = Math.round(SAMPLE_RATE * spec.secondMs / 1_000);
  const pcm = Buffer.alloc((firstFrames + gapFrames + secondFrames) * 2);
  let offset = 0;
  for (let i = 0; i < firstFrames; i += 1, offset += 2) pcm.writeInt16LE(toneSample(spec.first, i, firstFrames), offset);
  offset += gapFrames * 2;
  for (let i = 0; i < secondFrames; i += 1, offset += 2) pcm.writeInt16LE(toneSample(spec.second, i, secondFrames), offset);
  return pcm;
}

export function wrapTtsBackendWithSwitchEarcons(backend) {
  const wrapped = {
    name: backend.name,
    voice: backend.voice,
    gainDb: backend.gainDb,
    limiter: backend.limiter,
  };

  if (typeof backend.synthesize === "function") {
    wrapped.synthesize = async (text, format) => {
      const pcm = renderSwitchEarcon(text);
      if (pcm) {
        if (format?.sampleRate !== SAMPLE_RATE || format?.channels !== 1 || format?.bitsPerSample !== 16) {
          throw new Error("switch earcons require canonical mono 24000 Hz PCM16 output");
        }
        return pcm;
      }
      return backend.synthesize(text, format);
    };
  }

  if (typeof backend.stream === "function") {
    wrapped.stream = async (text, format, options = {}) => {
      const pcm = renderSwitchEarcon(text);
      if (!pcm) return backend.stream(text, format, options);
      if (format?.sampleRate !== SAMPLE_RATE || format?.channels !== 1 || format?.bitsPerSample !== 16) {
        throw new Error("switch earcons require canonical mono 24000 Hz PCM16 output");
      }
      const now = performance.now();
      const metrics = {
        requestStartedAt: now, responseHeadersAt: now, firstAudioByteAt: now,
        conditionerFirstOutputAt: now, completedAt: now, rawFloat32Bytes: 0,
        conditionedPcmBytes: pcm.length, producerQueueHighWaterBytes: pcm.length,
      };
      async function* audio() { yield pcm; }
      return { audio: audio(), metrics, streaming: true, cancel() {} };
    };
  }

  if (typeof backend.streamSegments === "function") {
    wrapped.streamSegments = (...args) => backend.streamSegments(...args);
  }

  return wrapped;
}
