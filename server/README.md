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

Install the initial VPS backend and normalizer:

```bash
sudo apt-get update
sudo apt-get install -y espeak-ng ffmpeg
```

Enable it in `.env` with `TTS_ENABLED=true`, `TTS_BACKEND=espeak`, `TTS_VOICE=en`, and an optional `TTS_RATE` (default 155 words/minute). Telegram is always answered first. The shared reply helper then strips HTML, normalizes common units, limits speech to 300 characters, and queues synthesis without awaiting playback. `/speak <text>` is a hidden bring-up command limited to 150 characters.

`espeak-ng --stdout -v <voice> -s <rate> <text>` is piped into `ffmpeg -i pipe:0 -f s16le -acodec pcm_s16le -ac 1 -ar 16000 pipe:1`. The backend boundary returns mono 16 kHz signed PCM16 little-endian and can be replaced without changing firmware or `/speaker`.

The VPS sends only `{type:"speaker_play", playback_id, sample_rate, channels, bits_per_sample, bytes}` over `/device`. Newo opens `/speaker` with the normal device headers plus `X-Newo-Playback-Id`. The server sends paced 2,048-byte binary PCM frames followed by `{type:"speaker_end", playback_id, bytes}` and Newo reports `speaker_complete` or `speaker_error` on `/device`. PCM never travels on the control socket.

Firmware keeps an 8,192-byte (256 ms) mono stream buffer, prebuffers 4,096 bytes (128 ms), duplicates samples into stereo I2S slots, and starts at 50% digital amplitude. The server sends the initial prebuffer immediately, then uses an absolute monotonic 32,000-byte/second PCM timeline without cumulative timer drift. Its dedicated TX pins are BCLK GPIO21, LRC GPIO47, and DOUT GPIO14. During actual playback it temporarily releases WakeNet and shows the existing SPEAKING animation; afterward it restores the unchanged OFF/ARMED choice and prior display state.

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
