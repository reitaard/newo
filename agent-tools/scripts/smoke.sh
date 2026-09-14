#!/usr/bin/env bash
set -euo pipefail

: "${AGENT_TOOLS_TOKEN:?Set AGENT_TOOLS_TOKEN first}"
base_url="${AGENT_TOOLS_BASE_URL:-http://127.0.0.1:8790}"

curl --fail --silent --show-error "$base_url/healthz"
curl --fail --silent --show-error -H "Authorization: Bearer $AGENT_TOOLS_TOKEN" "$base_url/v1/tools"
curl --fail --silent --show-error -H "Authorization: Bearer $AGENT_TOOLS_TOKEN" -H "Content-Type: application/json" \
  -d '{"query":"official Node.js documentation","max_results":2}' "$base_url/v1/tools/web/search"
curl --fail --silent --show-error -H "Authorization: Bearer $AGENT_TOOLS_TOKEN" -H "Content-Type: application/json" \
  -d '{"url":"https://nodejs.org/en/learn/getting-started/introduction-to-nodejs","max_chars":3000}' "$base_url/v1/tools/web/read"
