import { ServiceError } from "../errors.js";

const API_BASE = "https://api.tavily.com";

export class TavilyProvider {
  constructor({ apiKey, timeoutMs = 15_000, fetchImpl = fetch }) {
    this.name = "tavily";
    this.apiKey = apiKey;
    this.timeoutMs = timeoutMs;
    this.fetch = fetchImpl;
  }

  async request(path, body, outerSignal) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(new DOMException("Provider timeout", "TimeoutError")), this.timeoutMs);
    const abort = () => controller.abort(outerSignal.reason);
    outerSignal?.addEventListener("abort", abort, { once: true });
    try {
      const response = await this.fetch(`${API_BASE}${path}`, {
        method: "POST",
        headers: { authorization: `Bearer ${this.apiKey}`, "content-type": "application/json" },
        body: JSON.stringify(body),
        signal: controller.signal,
      });
      if (!response.ok) {
        if (response.status === 429) throw new ServiceError(503, "provider_rate_limited", "The web provider is temporarily rate limited");
        throw new ServiceError(502, "provider_error", `The web provider returned HTTP ${response.status}`);
      }
      return await response.json();
    } finally {
      clearTimeout(timeout);
      outerSignal?.removeEventListener("abort", abort);
    }
  }

  async search(input, signal) {
    const data = await this.request("/search", {
      query: input.query,
      topic: input.topic,
      search_depth: input.depth,
      max_results: input.maxResults,
      include_domains: input.includeDomains,
      exclude_domains: input.excludeDomains,
      time_range: input.timeRange,
      include_answer: false,
      include_raw_content: input.includeContent ? "markdown" : false,
      include_images: false,
      auto_parameters: false,
    }, signal);
    const results = (data.results || []).map((result) => ({
      title: result.title || null,
      url: result.url,
      snippet: result.content || "",
      content: input.includeContent ? (result.raw_content || null) : undefined,
      score: Number.isFinite(result.score) ? result.score : null,
      publishedAt: result.published_date || null,
    }));
    return { results, providerElapsedMs: Number.isFinite(data.response_time) ? Math.round(data.response_time * 1_000) : null };
  }

  async read(input, signal) {
    const data = await this.request("/extract", {
      urls: [input.url],
      extract_depth: input.depth,
      format: "markdown",
      include_images: false,
      include_favicon: false,
      timeout: Math.max(1, Math.min(60, this.timeoutMs / 1000)),
    }, signal);
    const result = data.results?.[0];
    if (!result) throw new ServiceError(502, "read_failed", "The provider could not read this URL");
    return { url: result.url || input.url, title: result.title || null, content: result.raw_content || "", publishedAt: result.published_date || null,
      providerElapsedMs: Number.isFinite(data.response_time) ? Math.round(data.response_time * 1_000) : null };
  }
}
