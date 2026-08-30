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
- `WS /voice` authenticated raw PCM audio stream for development ASR preparation
- `WS /speaker` authenticated server-to-device mono PCM playback stream
- `POST /telegram/webhook` enabled only when Telegram environment variables are configured

Telegram commands are allowlist-protected. The visible menu contains `/status` (quick status), `/health` (live device health), `/logs` (recent events), `/errors` (warnings/errors), and `/reboot` (acknowledged restart). Hidden aliases `/s`, `/h`, `/l`, `/e`, `/r`, `/ping`, and `/p` remain available; aliases and latency checks are not placed in the menu.

`/health` and `/logs` use the existing authenticated, correlated WSS request path. Device logs are a fixed 64-entry volatile RAM ring buffer; they remain available over USB Serial, are erased on reboot, contain no secrets, and are never returned by public HTTP `/health`.

Temporary structured operational tracing records safe Telegram update/message/chat IDs, normalized command name, handler invocation, request ID/type/expected response, single terminal settlement, and reply emission. It deliberately never records message text, tokens, secrets, or credentials; it is present to diagnose suspected duplicate replies.

## Environment

Copy `.env.example` to `.env` on the VPS and fill secrets there. Never commit `.env`.

### Speaker TTS

Install FFmpeg and keep eSpeak available as the explicit lightweight fallback:

```bash
sudo apt-get update
sudo apt-get install -y espeak-ng ffmpeg
```

Kokoro-82M runs persistently on CPU in a localhost-only container. Its cache survives container replacement and the English pipelines plus the three evaluation voices are preloaded:

```bash
cd /opt/newo/server
mkdir -p data/kokoro-cache
docker compose -f docker-compose.kokoro.yml up -d
curl -fsS http://127.0.0.1:8010/healthz
```

Configure Newo with `TTS_ENABLED=true`, `TTS_BACKEND=kokoro`, `TTS_VOICE=bf_emma`, `TTS_SAMPLE_RATE=24000`, `TTS_GAIN_DB=2`, and `KOKORO_BASE_URL=http://127.0.0.1:8010`. Voice selection is not hardwired; `bm_george` and `af_bella` can be selected through `TTS_VOICE` and a restart. Set `TTS_BACKEND=espeak`, `TTS_VOICE=en`, and normally `TTS_GAIN_DB=6` for the local fallback. An individual Kokoro failure is reported and never silently replaced with eSpeak.

Kokoro is requested with OpenAI-compatible `response_format=pcm`; only non-empty, even-length `application/octet-stream` is accepted. The native mono 24 kHz s16le is passed through a one-pass 110 Hz high-pass, conservative configurable gain, and 0.95 limiter with auto-gain disabled. Compressed responses are rejected. Final PCM peak/RMS, gain, limiter reach, clipping, byte count, and synthesis latency are logged.

Telegram is always answered first. Bounded text handling remains at 300 characters. `/speaker` persists automatic spoken replies on the VPS and in ESP NVS; `/speak <text>` (alias `/sp`) uses a temporary authenticated stream while automatic replies are OFF.

When Speaker is ON, Newo reuses one authenticated `/speaker` WSS. Existing `speaker_begin` / 1,024-byte binary PCM / `speaker_end` framing and `/device` completion events are unchanged. Receiver-driven `speaker_flow` credit targets 18,432 outstanding bytes (384 ms), has a hard ceiling of 21,504 bytes (448 ms), and stays below the ESP's 24,576-byte physical buffer. Firmware starts after 6,144 bytes (128 ms), duplicates mono samples to stereo I2S slots, and keeps real TX DMA-tail drain completion. Speaker OFF closes the stream and releases the 24,576-byte StreamBuffer/TLS resources. Heap, minimum heap, PSRAM, recovered memory, flow reports, buffer range, underruns, overflows, and drain time remain diagnostic outputs.

Primary Telegram mode commands are pure toggles: `/voice` (alias `/v`), `/speaker`, and `/eco`. Each waits for its applicable acknowledgement and returns a detailed subsystem status. The existing `/vs` alias remains a read-only detailed voice status. Speaker controls are `/volume`, `/volume <0-100>`, `/mute`, and `/speak <text>` (alias `/sp`; `/s` remains the established status alias). Existing operational aliases remain `/s`, `/h`, `/l`, `/e`, `/p`, `/r`, and `/f`; `/errors` behavior is unchanged.

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

Send continuous **binary WebSocket frames** containing raw PCM only. Default format is mono, 16 kHz, signed 16-bit little-endian. The server accepts arbitrary even-sized raw PCM chunks; it has no 640-byte assumption. Newo currently groups five contiguous internal 20 ms frames into a 3,200-byte / 100 ms message. `VOICE_MAX_CHUNK_BYTES` (64 KiB) limits one message and `VOICE_MAX_STREAM_BYTES` (default 10 minutes at 16 kHz mono 16-bit) bounds one connection. Do not send WAV headers, base64, separators, or binary payloads on `/device`.

The voice runtime logs connect/start/progress outcome metadata without logging audio. It calculates received PCM duration from the configured format. `VOICE_SAVE_WAV=false` is the default. Set it to `true` only during a local development capture; the finalized WAV file is written to `/tmp/newo-voice` by default and is never tracked by git.

`src/voice.js` defines the replaceable streaming ASR boundary: `createStream({ format, onEvent })`, `acceptAudio(chunk)`, and `stop()`. With `VOICE_ASR_BACKEND=sherpa`, `src/sherpa-worker.js` exclusively loads the native addon, owns recognizer streams, and performs synchronous decode. The Fastify thread only authenticates, receives bounded PCM, and forwards at most two undecoded chunks per connection; it closes a realtime stream rather than forming an unbounded JavaScript backlog. Sherpa defaults to `VOICE_SHERPA_MODEL=libri-giga`; `VOICE_ASR_BACKEND=null` retains the transport-only fallback.

The larger BPE model supports sherpa contextual biasing through `config/newo-hotwords.txt`: one phrase per line, initially `NEWO`, `HELLO`, `CHECK`, and `ONE TWO THREE`. `VOICE_ASR_HOTWORDS_SCORE=1.5` is intentionally conservative; changing the vocabulary or score requires a server restart. Biasing is enabled with modified beam search and affects recognition only—it does not rewrite transcript text or implement a wake word.

The streaming adapter decodes every ready chunk, emits only changed partial text, finalizes/reset streams at sherpa endpoints, and finalizes remaining text on disconnect. Events retain `{ type: "partial" | "final", text }` compatibility and add `stage: "partial" | "first_pass_final"`; a future rescorer can add `rescored_final` without changing the ESP protocol. With `VOICE_LIVE_TEST_MODE=true`, logs add received duration, first-partial timing, final timing, and final transcript for controlled repeated-phrase tests; WAV capture remains disabled unless explicitly enabled.

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
