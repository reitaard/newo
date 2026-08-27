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
- `WS /device` authenticated Newo device connection
- `POST /telegram/webhook` enabled only when Telegram environment variables are configured

## Environment

Copy `.env.example` to `.env` on the VPS and fill secrets there. Never commit `.env`.

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
