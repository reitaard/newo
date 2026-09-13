# Newo cloud server

The Newo cloud service is the public bridge between the ESP32-S3 device, Telegram, and later AI/backend services.

## Network shape

```text
Internet / Cloudflare
        |
        | HTTPS + WSS :443
        v
newo.reitaard.de
        |
        | reverse proxy
        v
127.0.0.1:8788
        |
        v
Newo Node server
```

The Node service binds to loopback by default. Port 8788 should not be exposed publicly.

## Routes

- `GET /` basic service identity
- `GET /health` cloud/device health JSON
- `WS /device` authenticated Newo device control/status connection
- `WS /voice` authenticated raw PCM audio stream for streaming ASR and one-turn assistant input
- `WS /speaker` authenticated server-to-device mono playback stream (native Opus, with PCM fallback)
- `POST /telegram/webhook` enabled only when Telegram environment variables are configured

Telegram commands are allowlist-protected. The visible menu contains `/status` (quick status), `/health` (live device health), `/logs` (recent events), `/errors` (warnings/errors), and `/reboot` (acknowledged restart). Hidden aliases `/s`, `/h`, `/l`, `/e`, `/r`, and `/pi` remain available; aliases and latency checks are not placed in the menu.

RF tracking uses the existing runtime-state and Telegram command architecture. `/track on|off|status|debug` controls and inspects tracking, while `/track_bg` starts tracking without a live-updating panel. Set `TRACK_TELEMETRY_TOKEN` to enable authenticated `POST /track/telemetry/v1` updates and tune freshness with `TRACK_TELEMETRY_STALE_MS` (default 6000 ms).

`/health` and `/logs` use the existing authenticated, correlated WSS request path. Device logs are a fixed 64-entry volatile RAM ring buffer; they remain available over USB Serial, are erased on reboot, contain no secrets, and are never returned by public HTTP `/health`.

Temporary structured operational tracing records safe Telegram update/message/chat IDs, normalized command name, handler invocation, request ID/type/expected response, single terminal settlement, and reply emission. It deliberately never records message text, tokens, secrets, or credentials; it is present to diagnose suspected duplicate replies.

## Environment

Copy `.env.example` to `.env` on the VPS and fill secrets there. Never commit `.env`.

### One-turn voice assistant

A finalized Sherpa transcript can make one bounded assistant request and, when it returns non-empty text, uses the existing Pocket-to-Opus `/speaker` path. Partials and cleanup finals never invoke the model, and one active assistant turn is allowed per device. The assistant does not retain conversation history, call tools, or create a new audio transport.

The written identity is **Newo**, while the spoken assistant name is **Neo**. The local WakeNet phrase is **Hi Wall-E**; its `wn9_hiwalle_tts2` asset lives in the ESP model partition, not in Sherpa or assistant settings. See `../docs/wake-word.md`.

The built-in `qwen3:0.6b` (`qwen`) profile preserves the private OpenAI-chat-compatible Qwen service and remains the default. The built-in `lfm2.5:8b` (`lfm`) profile owns the direct Ollama request, raw prompt, no-think filtering, sampling, timeout, and response bounds. Set `ASSISTANT_PROFILE=qwen` or `ASSISTANT_PROFILE=lfm` for the initial preference; Telegram `/profile_qwen`, `/profile_lfm`, and `/profile qwen|lfm` switch and persist it at runtime. `/profile` reports both the preferred and currently effective profile. Short aliases are `/p`, `/p_lfm`, and `/p_qwen`; `/ping` and `/pi` run the latency check. Hidden tuning controls are `/p_conf`, `/p_reset`, and `/p set|s <key> <value>`, where the bounded keys are `topk`, `topp`, `maxtoken`, `maxchars`, `timeout`, `rpenalty`, and `temp`. `/p s sysprompt` starts a two-step edit of the preferred profile's system prompt; the next plain-text message is saved, while `/cancel` exits without changing it. `/profile_tune` (alias `/pt`) also shows or applies the `fast`, `balanced`, `quality`, and `reset` persistent presets. Tuning never exposes or changes endpoints, API keys, providers, models, or fallback wiring in Telegram.

If the preferred LFM profile is unavailable because of a provider, network, HTTP, or timeout failure, one turn retries once with the complete Qwen profile. The LFM preference is retained and automatically recovered after its cooldown health check succeeds. Empty or low-quality answers do not trigger fallback. Ollama calls `/api/generate` directly; it does not proxy through another service or stream partial text to TTS.

### Speaker TTS

Install FFmpeg and keep eSpeak available as the explicit lightweight fallback:

```bash
sudo apt-get update
sudo apt-get install -y espeak-ng ffmpeg
```

OpenTTSGroup Kokoro-82M runs persistently on CPU in a digest-pinned, localhost-only container. Its cache survives container replacement and American English is preloaded:

```bash
cd /opt/newo/server
mkdir -p data/kokoro-cache
docker compose -f docker-compose.kokoro.yml up -d
curl -fsS http://127.0.0.1:8010/healthz
```

Pocket is the default backend: run the local-only warm service from the isolated Pocket virtual environment before starting Newo:

```bash
cd /opt/newo
HF_HOME=/opt/newo-pocket-tts-proto/cache/huggingface \
  /opt/newo-pocket-tts-proto/.venv/bin/python server/pocket-tts-service.py --port 8123
# In a second terminal, only proceed after this succeeds:
curl -fsS --retry 30 --retry-delay 1 http://127.0.0.1:8123/healthz
```

Set `TTS_ENABLED=true`, `TTS_BACKEND=pocket`, `SPEAKER_CODEC=opus`, and `POCKET_BASE_URL=http://127.0.0.1:8123`. Keep `TTS_VOICE=am_michael`: Pocket intentionally ignores it and stays on Michael, while it remains the ready Kokoro rollback voice. The service loads one CPU FP32 model, applies Pocket's dynamic INT8 quantization once (`torch.ao` when `torchao` is absent), and prepares only the evaluated `michael` state once. It binds only to loopback and serializes generation because Pocket is not thread-safe. No authentication is required for the released without-voice-cloning fallback model/state already present in the isolated cache.

Pocket returns native 24 kHz mono f32le in a genuinely streamed HTTP response. Newo validates the audio headers and incrementally carries 0–3 split float bytes, rejects non-finite/incomplete samples, clips safely to PCM16LE, and immediately feeds canonical PCM16 into the unchanged native Opus path (24 kbps, 40 ms/960-sample frames). It does not resample, invoke FFmpeg, write WAVs, collect the utterance, or apply the Kokoro high-pass/gain/limiter. `POCKET_REQUEST`, `POCKET_FIRST_PCM`, and `POCKET_DONE` logs distinguish request-to-first PCM, total generation, and generated audio duration. Pocket receives the natural bounded reply text intact; Newo's 28-character Kokoro opening segmentation is not used. Pocket retains its upstream roughly 50-token split policy; unusually long boundary-free text can produce its upstream skipped-word warning.

Install `config/newo-pocket-tts.service` as `/etc/systemd/system/newo-pocket-tts.service` and enable it. Pocket has no safe native cancellation for a generation blocked inside `next()`, so the worker watchdog exits after 15 seconds without model progress and systemd restarts it. This bounds a poisoned generation instead of allowing every later `/v`, Nano-trigger, or `/speak` request to wait behind the same lock. The Node client retries once, after a bounded 7.5-second restart window, only when no PCM has been emitted; it never repeats a partially delivered utterance. `POCKET_GENERATION_STALLED` and `POCKET_PRE_AUDIO_RETRY` identify the recovery; codec, Opus framing, and speaker conditioning are unchanged.

`TTS_BACKEND=kokoro` remains the rollback: configure `TTS_VOICE=am_michael`, `TTS_SPEED=1.0`, `TTS_GAIN_DB=2`, and `KOKORO_BASE_URL=http://127.0.0.1:8010`. Kokoro continues to use its existing FFmpeg high-pass/gain/limiter path. `TTS_BACKEND=espeak` remains a separate explicit fallback. A Pocket failure is reported to the job and never silently substitutes a different voice/backend.

Telegram network send starts first and TTS starts immediately afterward without awaiting Telegram. Neither operation blocks or poisons the other. Bounded text handling remains at 300 characters. `/speaker` persists automatic spoken replies on the VPS and in ESP NVS; `/speak <text>` (alias `/sp`) uses a temporary authenticated stream while automatic replies are OFF.

When Speaker is ON, Newo reuses one authenticated `/speaker` WSS. Connection readiness is unchanged; when Opus is requested, only per-playback codec selection waits briefly for the firmware's `speaker_ready` capabilities. Realtime playback uses explicit unknown-length `speaker_begin` framing and an exact final decoded-PCM byte count in `speaker_end`; known-length framing remains supported for eSpeak. Opus uses 24 kbps, 40 ms packets and a bounded 32-slot compressed queue in PSRAM. Firmware admits roughly 1,000 ms (25 packets) before decoding starts, while server credit targets roughly 1,200 ms of combined queued compressed audio plus decoded PCM, reports a 600 ms low-water reference, and enforces a 1,500 ms total ceiling. Once playback starts, I2S consumption is the clock: Opus sends are driven by admission/consumption credit and do not use the old wall-clock catch-up bursts. `speaker_flow` keeps compressed admission, decoded PCM receipt/buffering, and physical consumption as separate counters. PCM fallback retains its existing 14,336-byte receiver target, 8,192-byte network-flight limit, 21,504-byte outstanding ceiling, and 12,288-byte firmware prebuffer. Speaker OFF closes the stream and releases the bounded StreamBuffer/TLS resources. Heap, minimum heap, PSRAM, queue depth/high-water, flow reports, buffer range, underruns, overflows, decoder errors, and drain time remain diagnostic outputs.

`Hi Wall-E` starts an ARMED → STREAMING → ARMED turn through the existing `/voice` path. `/voice` (alias `/v`) remains the manual one-shot trigger and settles OFF. `/vs` reports live WakeNet state. `/owner_enroll`, `/owner_status`, and `/owner_cancel` manage the separate three-sample owner voiceprint. Speaker controls and all existing aliases are unchanged.

The first cloud bring-up only needs:

```text
HOST=127.0.0.1
PORT=8788
PUBLIC_BASE_URL=https://newo.reitaard.de
NEWO_DEVICE_ID=newo-01
NEWO_DEVICE_SECRET=<long-random-secret>
```

Telegram variables can remain blank until device transport is verified.

The WebSocket authenticates with these request headers:

```text
X-Newo-Device-Id: newo-01
Authorization: Bearer <NEWO_DEVICE_SECRET>
```

Secrets are never accepted in the URL query string.

## Voice stream preparation

`WS /voice` is a separate runtime from `/device`; it does not change device control, Telegram, or firmware behavior. It uses the same device-ID and bearer-secret headers as `/device`, so it is not a public unauthenticated endpoint:

```text
X-Newo-Device-Id: newo-01
Authorization: Bearer <NEWO_DEVICE_SECRET>
```

Send continuous **binary WebSocket frames** containing raw PCM only. Default format is mono, 16 kHz, signed 16-bit little-endian. The server does not assume 640-byte packets. It bundles direct ESP frames into 100 ms ASR worker calls and permits a bounded total pending-audio capacity of roughly 3,000 ms, including the active batch; sustained overflow still closes the stream instead of growing without limit. Newo's current firmware sends direct 20 ms PCM16 frames (320 mono samples / 640 bytes each) with no microphone PCM queue or bundling backlog. `VOICE_MAX_CHUNK_BYTES` (64 KiB) is the WebSocket parser ceiling and `VOICE_MAX_STREAM_BYTES` bounds one connection. Do not send WAV headers, base64, separators, or binary payloads on `/device`.

The voice runtime logs connect/start/progress outcome metadata without logging audio. A connection that sends no binary PCM within 2.5 seconds closes with `VOICE_NO_AUDIO_TIMEOUT`; first PCM cancels that timer and emits `VOICE_FIRST_AUDIO`. Firmware emits one `VOICE_PCM_HEALTH` amplitude summary after its first 25 frames without transmitting or logging samples. It calculates received PCM duration from the configured format. `VOICE_SAVE_WAV=false` is the default. Set it to `true` only during a local development capture; the finalized WAV file is written to `/tmp/newo-voice` by default and is never tracked by git.

`src/voice.js` defines the replaceable streaming ASR boundary: `createStream({ format, onEvent })`, `acceptAudio(chunk)`, and `stop()`. With `VOICE_ASR_BACKEND=sherpa`, `src/sherpa-worker.js` exclusively loads the native addon, owns the warm recognizer plus session streams, and performs synchronous decode. Startup primes the native decode kernels with a private silence stream and waits for `SHERPA_READY` before accepting connections; closing a real session removes only its stream, while application shutdown terminates the worker. The Fastify thread only authenticates and receives bounded PCM. It combines adjacent direct ESP frames into 100 ms (`VOICE_ASR_BATCH_MS`) worker calls, permits about 3,000 ms of bounded active-plus-pending audio to absorb transient worker latency, and closes on sustained overflow rather than forming an unbounded JavaScript backlog. Sherpa defaults to `VOICE_SHERPA_MODEL=libri-giga`; `VOICE_ASR_BACKEND=null` retains the transport-only fallback.

The larger BPE model supports Sherpa contextual biasing through `config/newo-hotwords.txt`. Production currently keeps this intentionally minimal as a single line, `NEO`, with `VOICE_ASR_HOTWORDS_SCORE=1.5` as a conservative default. Changing the vocabulary or score requires a server restart. Biasing affects recognition only—it does not rewrite transcript text and it does not implement or change the local WakeNet wake word.

The streaming adapter decodes every ready chunk, emits only changed partial text, finalizes/reset streams at sherpa endpoints, and finalizes remaining text on disconnect. Events retain `{ type: "partial" | "final", text }` compatibility and add `stage: "partial" | "first_pass_final"`; a future rescorer can add `rescored_final` without changing the ESP protocol. With `VOICE_LIVE_TEST_MODE=true`, logs add received duration, first-partial timing, final timing, and final transcript for controlled repeated-phrase tests; WAV capture remains disabled unless explicitly enabled.

Optional owner verification uses a second dedicated Sherpa worker and the English VoxCeleb 3D-Speaker CAM++ embedding model. PCM is submitted beside ASR, so ASR final can start the assistant without waiting for identity. `SPEAKER_IDENTITY` reports `owner` or `unknown`, cosine score, threshold, and availability; version one is informational and never blocks conversation. `/owner_enroll` collects three manual `/v` samples containing only “Hi Wall-E”, averages their normalized embeddings, and atomically stores the owner-only file under `data/voiceprints`. Enable it with the `SPEAKER_VERIFICATION_*` variables in `.env.example` after downloading the model described in `../docs/wake-word.md`.

Models are intentionally gitignored. To provision the default larger model on a new VPS:

```bash
cd /opt/newo/server
mkdir -p models
curl -fL -o /tmp/newo-model.tar.bz2 https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-en-2023-06-21.tar.bz2
tar -xjf /tmp/newo-model.tar.bz2 -C models
# Generate bpe.vocab from the matching upstream bpe.model with sherpa's export_bpe_vocab.py.
npm run voice:benchmark
```

Use `VOICE_SHERPA_MODEL=20m npm run voice:benchmark` for the 20M comparison and `VOICE_SHERPA_MODEL=libri-giga npm run voice:benchmark` for the default large-model benchmark.

For a local transport test after setting `NEWO_DEVICE_SECRET`, start the existing service with `npm start` (or restart its PM2 process), then connect a WebSocket client to `ws://127.0.0.1:8788/voice` with the headers above and send PCM frames. Use the public WSS address only through the existing reverse proxy. Stop the test by closing the WebSocket; the server logs final byte and duration totals.

## First VPS test

From `/opt/newo/server` after pulling the repository:

```bash
npm install
npm run check
cp .env.example .env
# edit .env and set NEWO_DEVICE_SECRET
npm start
```

In another shell:

```bash
curl http://127.0.0.1:8788/health
```

The expected initial device state is offline until ESP32 `newo_cloud` is added.

## Reverse proxy target

Configure the existing VPS reverse proxy for `newo.reitaard.de` to proxy normal HTTP and WebSocket upgrades to:

```text
http://127.0.0.1:8788
```

Keep Cloudflare proxied DNS in place. Public validation should include:

```bash
curl https://newo.reitaard.de/health
```

Do not register the Telegram webhook until `/health` and the device WebSocket path are working through HTTPS/WSS.

## Telegram

When Telegram bring-up begins, set the VPS-only values:

```text
TELEGRAM_BOT_TOKEN=
TELEGRAM_WEBHOOK_SECRET=
TELEGRAM_ALLOWED_USER_IDS=
TELEGRAM_ALLOWED_CHAT_IDS=
```

When a bot token is configured, a webhook secret is required. The webhook handler uses grammY's secret-token verification. Commands are rejected unless their user or chat ID appears in the configured allowlists.

Device requests use unique in-memory request IDs and a five-second timeout. Responses are accepted only from the authenticated device connection and only when their request type and request ID match. Pending requests are cleared on timeout, disconnect, or shutdown.

Wi-Fi provisioning is BLE-only and is never exposed through Telegram. A reboot is scheduled by firmware only after its correlated `reboot_ack` frame has been accepted for transmission. After acknowledgement, the server keeps the requesting chat/message/device IDs in memory for 60 seconds. Reconnection deletes the temporary “Restarting Newo.” message when possible and sends “Newo is back online.”; timeout edits the temporary message instead. Intentional reboot disconnects suppress generic connectivity notifications.

The public `/health` response omits Wi-Fi telemetry such as SSID; authorized Telegram status may include SSID and RSSI.

Connectivity notifications are sent asynchronously to configured allowed chat IDs after a 12-second offline grace period. Initial state and server shutdown are suppressed so PM2 restarts do not create misleading notifications.
