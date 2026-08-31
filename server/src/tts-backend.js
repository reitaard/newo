import { EspeakTtsBackend, KokoroTtsBackend, ESPEAK_GAIN_DB, SPEAKER_GAIN_DB } from "./tts.js";
import { PocketTtsBackend } from "./pocket-tts.js";

/** Pure backend selection keeps Pocket default and Kokoro rollback testable without booting the server. */
export function createTtsBackend(env, logger) {
  const voice = env.TTS_BACKEND === "pocket" ? "michael" : env.TTS_VOICE ?? (env.TTS_BACKEND === "kokoro" ? "am_michael" : "en");
  const gainDb = env.TTS_GAIN_DB ?? (env.TTS_BACKEND === "kokoro" ? SPEAKER_GAIN_DB : env.TTS_BACKEND === "pocket" ? 0 : ESPEAK_GAIN_DB);
  if (env.TTS_BACKEND === "pocket") return new PocketTtsBackend({
    baseUrl: env.POCKET_BASE_URL, voice, requestTimeoutMs: env.POCKET_REQUEST_TIMEOUT_MS,
    streamNoProgressMs: env.POCKET_STREAM_NO_PROGRESS_MS, streamAbsoluteMs: env.POCKET_STREAM_ABSOLUTE_MS,
    maxPcmBytes: env.TTS_MAX_PCM_BYTES, logger,
  });
  if (env.TTS_BACKEND === "kokoro") return new KokoroTtsBackend({
    baseUrl: env.KOKORO_BASE_URL, voice, speed: env.TTS_SPEED, requestTimeoutMs: env.KOKORO_REQUEST_TIMEOUT_MS,
    streamNoProgressMs: env.KOKORO_STREAM_NO_PROGRESS_MS, streamAbsoluteMs: env.KOKORO_STREAM_ABSOLUTE_MS,
    gainDb, maxPcmBytes: env.TTS_MAX_PCM_BYTES, logger,
  });
  return new EspeakTtsBackend({ voice, rate: env.TTS_RATE, gainDb, maxPcmBytes: env.TTS_MAX_PCM_BYTES });
}
