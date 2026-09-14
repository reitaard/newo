import { createHash, randomUUID, timingSafeEqual } from "node:crypto";
import { createServer } from "node:http";
import { ServiceError, publicError } from "./errors.js";

const jsonHeaders = { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" };

function send(response, status, body, extraHeaders = {}) {
  response.writeHead(status, { ...jsonHeaders, ...extraHeaders });
  response.end(JSON.stringify(body));
}

function authenticated(header, expected) {
  const supplied = header?.startsWith("Bearer ") ? header.slice(7) : "";
  const a = createHash("sha256").update(supplied).digest();
  const b = createHash("sha256").update(expected).digest();
  return Boolean(supplied) && timingSafeEqual(a, b);
}

async function readJson(request, limit) {
  const chunks = [];
  let size = 0;
  for await (const chunk of request) {
    size += chunk.length;
    if (size > limit) throw new ServiceError(413, "request_too_large", "Request body exceeds the configured limit");
    chunks.push(chunk);
  }
  try {
    return JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}");
  } catch {
    throw new ServiceError(400, "invalid_json", "Request body must be valid JSON");
  }
}

function text(value, name, { min = 1, max = 500 } = {}) {
  if (typeof value !== "string" || value.trim().length < min || value.length > max) {
    throw new ServiceError(400, "invalid_request", `${name} must be a string between ${min} and ${max} characters`);
  }
  return value.trim();
}

function choice(value, name, allowed, fallback) {
  if (value === undefined) return fallback;
  if (!allowed.includes(value)) throw new ServiceError(400, "invalid_request", `${name} must be one of: ${allowed.join(", ")}`);
  return value;
}

function boundedInteger(value, name, fallback, min, max) {
  if (value === undefined) return fallback;
  if (!Number.isInteger(value) || value < min || value > max) throw new ServiceError(400, "invalid_request", `${name} must be an integer from ${min} to ${max}`);
  return value;
}

function domains(value, name) {
  if (value === undefined) return [];
  if (!Array.isArray(value) || value.length > 20 || value.some((item) => typeof item !== "string" || item.length > 253)) {
    throw new ServiceError(400, "invalid_request", `${name} must be an array of at most 20 domain names`);
  }
  return value;
}

function parseSearch(body) {
  return {
    query: text(body.query, "query"),
    maxResults: boundedInteger(body.max_results, "max_results", 5, 1, 10),
    depth: choice(body.depth, "depth", ["basic", "advanced"], "basic"),
    topic: choice(body.topic, "topic", ["general", "news"], "general"),
    timeRange: choice(body.time_range, "time_range", ["day", "week", "month", "year"], undefined),
    includeDomains: domains(body.include_domains, "include_domains"),
    excludeDomains: domains(body.exclude_domains, "exclude_domains"),
    includeContent: body.include_content === true,
  };
}

function parseRead(body) {
  const raw = text(body.url, "url", { max: 2048 });
  let url;
  try { url = new URL(raw); } catch { throw new ServiceError(400, "invalid_request", "url must be a valid HTTP or HTTPS URL"); }
  if (!["http:", "https:"].includes(url.protocol) || url.username || url.password) throw new ServiceError(400, "invalid_request", "url must be a public HTTP or HTTPS URL without credentials");
  return {
    url: url.toString(),
    depth: choice(body.depth, "depth", ["basic", "advanced"], "basic"),
    maxChars: boundedInteger(body.max_chars, "max_chars", 30_000, 1_000, 100_000),
  };
}

function toolManifest(provider) {
  return {
    version: "v1",
    provider: provider.name,
    tools: [
      { id: "web.search", method: "POST", path: "/v1/tools/web/search", description: "Search the web and return ranked source evidence without a generated answer", read_only: true },
      { id: "web.read", method: "POST", path: "/v1/tools/web/read", description: "Extract readable content from one HTTP or HTTPS URL", read_only: true },
    ],
  };
}

export function createAgentToolsService({ config, provider, logger = console }) {
  const rate = new Map();
  function consume(key, now = Date.now()) {
    let entry = rate.get(key);
    if (!entry || now >= entry.resetAt) entry = { count: 0, resetAt: now + config.rateLimitWindowMs };
    entry.count += 1;
    rate.set(key, entry);
    return { allowed: entry.count <= config.rateLimitRequests, remaining: Math.max(0, config.rateLimitRequests - entry.count), resetAt: entry.resetAt };
  }

  return createServer(async (request, response) => {
    const requestId = request.headers["x-request-id"]?.toString().slice(0, 128) || randomUUID();
    const started = performance.now();
    response.setHeader("x-request-id", requestId);
    let tool = null;
    try {
      const url = new URL(request.url, "http://localhost");
      if (request.method === "GET" && url.pathname === "/healthz") {
        return send(response, 200, { status: "ok", service: "agent-tools", provider: provider.name, configured: true });
      }
      if (!authenticated(request.headers.authorization, config.callerToken)) throw new ServiceError(401, "unauthorized", "A valid bearer token is required");
      const limit = consume(request.socket.remoteAddress || "unknown");
      const limitHeaders = { "x-ratelimit-limit": String(config.rateLimitRequests), "x-ratelimit-remaining": String(limit.remaining), "x-ratelimit-reset": String(Math.ceil(limit.resetAt / 1000)) };
      if (!limit.allowed) throw new ServiceError(429, "rate_limited", "Rate limit exceeded");
      if (request.method === "GET" && url.pathname === "/v1/tools") return send(response, 200, toolManifest(provider), limitHeaders);
      const controller = new AbortController();
      request.once("aborted", () => controller.abort());
      const fetchedAt = new Date().toISOString();
      if (request.method === "POST" && url.pathname === "/v1/tools/web/search") {
        tool = "web.search";
        const input = parseSearch(await readJson(request, config.bodyLimitBytes));
        const outcome = await provider.search(input, controller.signal);
        const results = Array.isArray(outcome) ? outcome : outcome.results;
        const body = {
          request_id: requestId,
          tool,
          provider: provider.name,
          query: input.query,
          results: results.map((item) => ({
            title: item.title,
            url: item.url,
            snippet: item.snippet,
            ...(item.content !== undefined ? { content: item.content } : {}),
            score: item.score,
            published_at: item.publishedAt,
            fetched_at: fetchedAt,
          })),
          result_count: results.length,
          provider_elapsed_ms: Array.isArray(outcome) ? null : outcome.providerElapsedMs,
          elapsed_ms: Math.round(performance.now() - started),
        };
        send(response, 200, body, limitHeaders);
        logger.info(JSON.stringify({ event: "tool_request", request_id: requestId, tool, provider: provider.name, status: 200, elapsed_ms: body.elapsed_ms, result_count: results.length }));
        return;
      }
      if (request.method === "POST" && url.pathname === "/v1/tools/web/read") {
        tool = "web.read";
        const input = parseRead(await readJson(request, config.bodyLimitBytes));
        const result = await provider.read(input, controller.signal);
        const truncated = result.content.length > input.maxChars;
        const body = { request_id: requestId, tool, provider: provider.name, url: result.url, title: result.title, content: result.content.slice(0, input.maxChars), published_at: result.publishedAt, fetched_at: fetchedAt, truncated,
          provider_elapsed_ms: result.providerElapsedMs ?? null, elapsed_ms: Math.round(performance.now() - started) };
        send(response, 200, body, limitHeaders);
        logger.info(JSON.stringify({ event: "tool_request", request_id: requestId, tool, provider: provider.name, status: 200, elapsed_ms: body.elapsed_ms, result_count: 1, truncated }));
        return;
      }
      throw new ServiceError(404, "not_found", "Route not found");
    } catch (rawError) {
      const error = publicError(rawError);
      const elapsed = Math.round(performance.now() - started);
      send(response, error.status, { error: { code: error.code, message: error.message, request_id: requestId, ...(error.details ? { details: error.details } : {}) } });
      logger.warn(JSON.stringify({ event: "tool_request", request_id: requestId, tool, provider: provider.name, status: error.status, error_code: error.code, elapsed_ms: elapsed }));
    }
  });
}
