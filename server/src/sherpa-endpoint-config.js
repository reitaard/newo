export const DEFAULT_SHERPA_ENDPOINTS = Object.freeze({
  rule1Seconds: 2.0,
  rule2Seconds: 1.0,
  rule3Seconds: 20,
});

function parseBounded(name, raw, fallback, min, max) {
  if (raw === undefined || raw === null || String(raw).trim() === "") return fallback;
  const value = Number(raw);
  if (!Number.isFinite(value) || value < min || value > max) {
    throw new Error(`${name} must be a number between ${min} and ${max}`);
  }
  return value;
}

export function resolveSherpaEndpointConfig(env = process.env, defaults = DEFAULT_SHERPA_ENDPOINTS) {
  return Object.freeze({
    rule1Seconds: parseBounded("VOICE_ASR_ENDPOINT_RULE1_S", env.VOICE_ASR_ENDPOINT_RULE1_S, defaults.rule1Seconds, 0.1, 10),
    rule2Seconds: parseBounded("VOICE_ASR_ENDPOINT_RULE2_S", env.VOICE_ASR_ENDPOINT_RULE2_S, defaults.rule2Seconds, 0.1, 10),
    rule3Seconds: parseBounded("VOICE_ASR_ENDPOINT_RULE3_S", env.VOICE_ASR_ENDPOINT_RULE3_S, defaults.rule3Seconds, 1, 120),
  });
}
