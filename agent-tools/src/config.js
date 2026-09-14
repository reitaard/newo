function integer(value, fallback, min, max) {
  const parsed = Number.parseInt(value ?? "", 10);
  return Number.isInteger(parsed) && parsed >= min && parsed <= max ? parsed : fallback;
}

export function loadConfig(env = process.env) {
  return {
    host: env.HOST || "127.0.0.1",
    port: integer(env.PORT, 8790, 1, 65535),
    callerToken: env.AGENT_TOOLS_TOKEN || "",
    tavilyApiKey: env.TAVILY_API_KEY || "",
    providerTimeoutMs: integer(env.PROVIDER_TIMEOUT_MS, 15_000, 1_000, 60_000),
    bodyLimitBytes: integer(env.REQUEST_BODY_LIMIT_BYTES, 32_768, 1_024, 1_048_576),
    rateLimitRequests: integer(env.RATE_LIMIT_REQUESTS, 30, 1, 10_000),
    rateLimitWindowMs: integer(env.RATE_LIMIT_WINDOW_MS, 60_000, 1_000, 3_600_000),
  };
}

export function assertConfig(config) {
  if (config.callerToken.length < 24) throw new Error("AGENT_TOOLS_TOKEN must contain at least 24 characters");
  if (!config.tavilyApiKey) throw new Error("TAVILY_API_KEY is required");
}
