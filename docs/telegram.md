# Telegram integration

## Decision

Telegram is integrated through the Newo cloud service on the VPS, not by putting the Telegram bot token on the ESP32.

```text
Telegram
   |
   | HTTPS webhook
   v
Newo VPS / domain
   |
   | authenticated outbound device channel
   v
Newo ESP32-S3
```

The ESP32 remains a device endpoint. `newo_cloud` will maintain an outbound authenticated TLS connection to the VPS. The Telegram service receives bot updates, validates authorization, translates commands into Newo device messages, and sends responses back through the Telegram Bot API.

## Why this layout

- the Telegram bot token never needs to be stored in ESP flash;
- Newo works behind NAT, DHCP, hotel/office client isolation, and changing Wi-Fi networks;
- Telegram does not need inbound access to the ESP32;
- Telegram command processing cannot block future audio, camera, display, or wake-word tasks on the ESP32;
- the same cloud/device channel can later carry web UI, AI, telemetry, OTA metadata, and other integrations;
- bot authorization and command policy live on the VPS where they are easier to update safely.

## VPS stack

Use Node.js with:

- `grammy` for Telegram Bot API handling;
- `fastify` as the HTTP/webhook server;
- `ws` for the persistent Newo device WebSocket channel;
- `zod` for validation of environment/config values and typed command payloads.

Node 22+ provides native `fetch`, so no extra HTTP client is needed for ordinary backend HTTP calls.

grammY supports webhook operation through `webhookCallback`, including Fastify adapters and Telegram webhook secret-token validation. `ws` is a focused WebSocket implementation that supports attaching to an existing HTTP/S server and provides heartbeat/client-authentication patterns suitable for the device channel.

Initial VPS dependency install:

```bash
npm install grammy fastify ws zod
```

Development dependencies can be added once the backend source tree is created (TypeScript, linting, tests) rather than being required for ESP32 Phase 1.

## ESP32 cloud transport

For the ESP32-side persistent cloud channel, use the `WebSockets` Arduino library by Markus Sattler / Links2004, version 2.7.2 or later compatible 2.7.x. The project supports ESP32 secure WebSocket clients (`wss://`); release 2.7.1 added Arduino >=3.x custom-network-client support and 2.7.2 is the current release selected for Newo bring-up.

Arduino Library Manager package:

```text
WebSockets by Markus Sattler — 2.7.2
```

This dependency belongs to `newo_cloud` and remains separate from BLE Wi-Fi provisioning.

TLS certificate validation must stay enabled in production. Do not use an insecure TLS mode as the normal Newo configuration.

## Telegram update transport

Production should use a Telegram webhook terminating on the VPS/domain over HTTPS. Telegram supports `setWebhook`, requires HTTPS for the standard hosted Bot API webhook flow, and supports ports 443, 80, 88, and 8443. Newo should normally use 443.

Configure a `secret_token` when registering the webhook. Telegram then includes it in the `X-Telegram-Bot-Api-Secret-Token` request header and the VPS must reject requests whose value does not match.

Telegram documents long polling through `getUpdates` as the alternative. It is useful for local development, but a bot cannot use `getUpdates` while a webhook is configured.

## Secrets

Never commit any of these values:

- Telegram bot token
- Telegram webhook secret
- allowed Telegram user/chat IDs if they are considered private configuration
- Newo device credentials
- VPS API keys or private certificates

Store Telegram secrets as VPS environment variables or in the VPS secret manager.

Suggested environment names:

```text
TELEGRAM_BOT_TOKEN=
TELEGRAM_WEBHOOK_SECRET=
TELEGRAM_ALLOWED_USER_IDS=
TELEGRAM_ALLOWED_CHAT_IDS=
NEWO_DEVICE_SECRET=
```

The ESP32 receives a separate per-device identity/credential. It does **not** receive `TELEGRAM_BOT_TOKEN`.

## Initial command surface

The visible menu is deliberately limited and remains behind the VPS allowlist:

- `/status` — request fresh online/offline state, SSID/RSSI, uptime, and firmware
- `/reboot` — request restart and report success only after the ESP32 sends `reboot_ack`

`/start` returns a one-line online/offline greeting. `/ping` remains functional as a hidden diagnostic command that waits for the correlated ESP32 `pong` and reports round-trip latency.

The server correlates device requests by unique `request_id` values. Each pending request records its expected response type and start time, expires after five seconds, and is removed on response, timeout, disconnect, or shutdown. A response from another socket or with a mismatched type cannot resolve it.

`/status` requests fresh telemetry whenever the device is online. If that request times out, a concise cached snapshot is clearly labelled. `/reboot` sends `reboot`; firmware sends `reboot_ack`, allows the WSS frame to drain, and only then schedules restart. The server replies “Restarting Newo.” and tracks that chat/message/device in memory for 60 seconds. On authenticated reconnect it best-effort deletes that message and sends “Newo is back online.” A timeout edits the existing message, and intentional reboot disconnects do not emit generic offline/online notifications.

Wi-Fi provisioning is BLE-only and cannot be initiated or receive credentials through Telegram. Commands for camera/audio capture or OTA should be added only after authorization and transport are hardware-tested.

The Telegram command menu contains only `status` and `reboot`.

## Authorization

Receiving a valid Telegram webhook is not enough to authorize a device command. The VPS must also check the sending Telegram user/chat against an allowlist before forwarding anything to Newo.

The VPS sends connectivity notifications only to configured allowed chat IDs. A disconnect must remain outside a 12-second grace period before `Newo went offline.` is sent; a reconnect within that grace cancels it. A later reconnect after an announced outage sends a back-online message with the offline duration. Initial connection and graceful server shutdown do not generate notifications.

Suggested order for every update:

```text
validate webhook secret
  -> parse Telegram update
  -> verify allowed user/chat
  -> validate command and arguments
  -> forward typed command to Newo
  -> wait for/stream device response
  -> reply through Telegram Bot API
```

## Bot setup checks

Before wiring the bot into Newo, verify the token against the official Bot API `getMe` method. When the VPS endpoint is ready, register its HTTPS URL with `setWebhook`, include a webhook secret, then verify webhook state with `getWebhookInfo`.

Do not paste the bot token into source files, screenshots, GitHub issues, or Serial Monitor output.

## ESP32 Telegram-library decision

No Telegram-specific Arduino library is required for the planned architecture.

`UniversalTelegramBot` is widely used but remains at library version 1.3.0 and has open maintenance/ArduinoJson 7 concerns. `AsyncTelegram2` has explicit ArduinoJson 7 compatibility and is a stronger option if a future standalone ESP-to-Telegram fallback is ever needed. Neither is required for Newo's primary design because Telegram terminates at the VPS.

This keeps the ESP32 firmware focused on `newo_cloud` rather than binding the assistant to Telegram-specific code.

## Upstream references

- Telegram Bot API: https://core.telegram.org/bots/api
- Telegram bot tutorial: https://core.telegram.org/bots/tutorial
- Telegram webhook guide: https://core.telegram.org/bots/webhooks
- Telegram bot FAQ: https://core.telegram.org/bots/faq
- grammY: https://grammy.dev/
- grammY VPS/webhook guide: https://grammy.dev/hosting/vps
- ws: https://github.com/websockets/ws
- ESP32 WebSockets: https://github.com/Links2004/arduinoWebSockets
- AsyncTelegram2: https://github.com/cotestatnt/AsyncTelegram2
- Universal Arduino Telegram Bot: https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot
