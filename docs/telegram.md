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

Bring-up commands should be deliberately small:

- `/start` — identify the bot and Newo connection state
- `/status` — online/offline, Wi-Fi RSSI, uptime, firmware/device information
- `/ping` — round-trip test from Telegram to VPS to Newo and back

Commands that change device state, reboot firmware, capture camera/audio, or trigger OTA should be added only after authorization and cloud transport are tested.

## Authorization

Receiving a valid Telegram webhook is not enough to authorize a device command. The VPS must also check the sending Telegram user/chat against an allowlist before forwarding anything to Newo.

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

## ESP32 library decision

No Telegram-specific Arduino library is required for the planned architecture.

`UniversalTelegramBot` is widely used but remains at library version 1.3.0 and has open maintenance/ArduinoJson 7 concerns. `AsyncTelegram2` has explicit ArduinoJson 7 compatibility and is a stronger option if a future standalone ESP-to-Telegram fallback is ever needed. Neither is required for Newo's primary design because Telegram terminates at the VPS.

This keeps the ESP32 firmware focused on `newo_cloud` rather than binding the assistant to Telegram-specific code.

## Upstream references

- Telegram Bot API: https://core.telegram.org/bots/api
- Telegram bot tutorial: https://core.telegram.org/bots/tutorial
- Telegram webhook guide: https://core.telegram.org/bots/webhooks
- Telegram bot FAQ: https://core.telegram.org/bots/faq
- AsyncTelegram2: https://github.com/cotestatnt/AsyncTelegram2
- Universal Arduino Telegram Bot: https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot
