import assert from "node:assert/strict";
import { once } from "node:events";
import test from "node:test";
import { createAgentToolsService } from "../src/service.js";

const config = { callerToken: "a-valid-test-token-that-is-long-enough", bodyLimitBytes: 1024, rateLimitRequests: 20, rateLimitWindowMs: 60_000 };
const quiet = { info() {}, warn() {} };

async function fixture(provider = {}) {
  const fake = {
    name: "fake",
    async search(input) { return [{ title: "Result", url: "https://example.com", snippet: input.query, score: 0.9, publishedAt: null }]; },
    async read(input) { return { url: input.url, title: "Page", content: "content", publishedAt: null }; },
    ...provider,
  };
  const server = createAgentToolsService({ config, provider: fake, logger: quiet });
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const base = `http://127.0.0.1:${server.address().port}`;
  return { server, base };
}

function auth() { return { authorization: `Bearer ${config.callerToken}`, "content-type": "application/json" }; }

test("health is public but tool catalog requires caller auth", async (t) => {
  const { server, base } = await fixture(); t.after(() => server.close());
  assert.equal((await fetch(`${base}/healthz`)).status, 200);
  const denied = await fetch(`${base}/v1/tools`);
  assert.equal(denied.status, 401);
  assert.equal((await denied.json()).error.code, "unauthorized");
  const catalog = await fetch(`${base}/v1/tools`, { headers: auth() });
  assert.equal(catalog.status, 200);
  assert.deepEqual((await catalog.json()).tools.map((tool) => tool.id), ["web.search", "web.read"]);
});

test("search returns provider-neutral evidence without generated answer", async (t) => {
  let received;
  const { server, base } = await fixture({ async search(input) { received = input; return [{ title: "Docs", url: "https://example.com/docs", snippet: "Evidence", score: 0.8, publishedAt: "2026-01-01" }]; } });
  t.after(() => server.close());
  const response = await fetch(`${base}/v1/tools/web/search`, { method: "POST", headers: auth(), body: JSON.stringify({ query: "test query", max_results: 3, depth: "advanced" }) });
  const body = await response.json();
  assert.equal(response.status, 200);
  assert.equal(received.maxResults, 3);
  assert.equal(received.depth, "advanced");
  assert.equal(body.result_count, 1);
  assert.equal("answer" in body, false);
  assert.equal(body.results[0].published_at, "2026-01-01");
  assert.equal("publishedAt" in body.results[0], false);
  assert.match(body.results[0].fetched_at, /^\d{4}-/);
});

test("read validates URLs and truncates output deterministically", async (t) => {
  const { server, base } = await fixture({ async read(input) { return { url: input.url, title: null, content: "x".repeat(1500), publishedAt: null }; } });
  t.after(() => server.close());
  const bad = await fetch(`${base}/v1/tools/web/read`, { method: "POST", headers: auth(), body: JSON.stringify({ url: "file:///etc/passwd" }) });
  assert.equal(bad.status, 400);
  const good = await fetch(`${base}/v1/tools/web/read`, { method: "POST", headers: auth(), body: JSON.stringify({ url: "https://example.com/a", max_chars: 1000 }) });
  const body = await good.json();
  assert.equal(good.status, 200);
  assert.equal(body.content.length, 1000);
  assert.equal(body.truncated, true);
});

test("provider failures use structured errors and do not leak secrets", async (t) => {
  const { server, base } = await fixture({ async search() { throw new Error("secret upstream detail"); } });
  t.after(() => server.close());
  const response = await fetch(`${base}/v1/tools/web/search`, { method: "POST", headers: auth(), body: JSON.stringify({ query: "test" }) });
  const raw = await response.text();
  assert.equal(response.status, 502);
  assert.equal(raw.includes("secret upstream detail"), false);
  assert.equal(JSON.parse(raw).error.code, "provider_error");
});

test("oversized request bodies are rejected", async (t) => {
  const { server, base } = await fixture(); t.after(() => server.close());
  const response = await fetch(`${base}/v1/tools/web/search`, { method: "POST", headers: auth(), body: JSON.stringify({ query: "x".repeat(1500) }) });
  assert.equal(response.status, 413);
});

test("rate limits authenticated callers", async (t) => {
  const limited = { ...config, rateLimitRequests: 1 };
  const provider = { name: "fake", async search() { return []; }, async read() { return {}; } };
  const server = createAgentToolsService({ config: limited, provider, logger: quiet });
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  t.after(() => server.close());
  const base = `http://127.0.0.1:${server.address().port}`;
  assert.equal((await fetch(`${base}/v1/tools`, { headers: auth() })).status, 200);
  const response = await fetch(`${base}/v1/tools`, { headers: auth() });
  assert.equal(response.status, 429);
  assert.equal((await response.json()).error.code, "rate_limited");
});
