# Agent Tools

Standalone, provider-neutral HTTP tools for authorized agents. This service is intentionally independent from Newo's assistant profiles and voice runtime.

## API

- `GET /healthz` — public process readiness (never returns secrets)
- `GET /v1/tools` — authenticated tool catalog
- `POST /v1/tools/web/search` — ranked web evidence
- `POST /v1/tools/web/read` — extracted content for one URL

All `/v1/tools` routes require `Authorization: Bearer $AGENT_TOOLS_TOKEN`. Errors use `{ "error": { "code", "message", "request_id" } }`.

Search accepts `query`, `max_results` (1–10), `depth` (`basic|advanced`), `topic` (`general|news`), `time_range`, domain filters, and `include_content`. It never asks Tavily for a generated answer. Read accepts `url`, `depth`, and `max_chars`.

## Local verification

```bash
cp .env.example .env
# Set private values in .env, then export them or source the file.
npm run check
npm test
npm start
```

## VPS layout

Runtime files live in `/srv/agent-tools`, private configuration in `/srv/agent-tools/.env`, and PM2 runs the separate `agent-tools` app from `ecosystem.config.cjs`. Node loads the private file through `--env-file`; it is not stored in the PM2 config or Git. The default listener is loopback-only on port 8790; expose it only through an authenticated trusted gateway or private network.

Smoke test after startup:

```bash
cd /srv/agent-tools
set -a && source .env && set +a
./scripts/smoke.sh
```
