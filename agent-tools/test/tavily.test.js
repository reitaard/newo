import assert from "node:assert/strict";
import test from "node:test";
import { TavilyProvider } from "../src/providers/tavily.js";

test("Tavily search adapter disables provider answers and maps evidence", async () => {
  let request;
  const provider = new TavilyProvider({ apiKey: "hidden", fetchImpl: async (url, options) => {
    request = { url, options, body: JSON.parse(options.body) };
    return new Response(JSON.stringify({ results: [{ title: "A", url: "https://a.test", content: "summary", raw_content: "full", score: 0.7, published_date: "2026-01-02" }] }), { status: 200 });
  } });
  const results = await provider.search({ query: "q", topic: "general", depth: "basic", maxResults: 2, includeDomains: [], excludeDomains: [], includeContent: true }, new AbortController().signal);
  assert.equal(request.url, "https://api.tavily.com/search");
  assert.equal(request.body.include_answer, false);
  assert.equal(request.body.auto_parameters, false);
  assert.equal(request.options.headers.authorization, "Bearer hidden");
  assert.equal(results[0].content, "full");
});

test("Tavily read adapter uses extract with a single URL", async () => {
  let body;
  const provider = new TavilyProvider({ apiKey: "hidden", fetchImpl: async (_url, options) => {
    body = JSON.parse(options.body);
    return new Response(JSON.stringify({ results: [{ url: "https://a.test/", raw_content: "page" }] }), { status: 200 });
  } });
  const result = await provider.read({ url: "https://a.test/", depth: "basic" }, new AbortController().signal);
  assert.deepEqual(body.urls, ["https://a.test/"]);
  assert.equal(body.format, "markdown");
  assert.equal(result.content, "page");
});

test("Tavily adapter aborts a stalled provider request", async () => {
  const provider = new TavilyProvider({ apiKey: "hidden", timeoutMs: 5, fetchImpl: async (_url, options) => {
    await new Promise((resolve, reject) => {
      options.signal.addEventListener("abort", () => reject(options.signal.reason), { once: true });
    });
  } });
  await assert.rejects(
    provider.search({ query: "q", topic: "general", depth: "basic", maxResults: 1, includeDomains: [], excludeDomains: [], includeContent: false }, new AbortController().signal),
    (error) => error.name === "TimeoutError",
  );
});
