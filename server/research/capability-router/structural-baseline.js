import { readFileSync } from "node:fs";
import { performance } from "node:perf_hooks";

const corpusPath = process.argv[2] || new URL("./corpus.jsonl", import.meta.url).pathname;
const rows = readFileSync(corpusPath, "utf8").split(/\r?\n/).filter(Boolean).map(JSON.parse);

const currencyCode = "(?:USD|EUR|GBP|JPY|KHR|AUD|CAD|CNY|RMB|THB|SGD|KRW|dollars?|euros?|pounds?|yen|riel)";
const unit = "(?:mm|cm|m|km|inches?|feet|foot|yards?|miles?|grams?|kg|kilograms?|pounds?|lb|liters?|litres?|ml|milliliters?|cups?|celsius|fahrenheit|km\/h|mph|meters? per second)";

function result(primary, { capabilities = primary ? [primary] : [], sourceNeed = null, abstain = false, scores = null } = {}) {
  return { primary, capabilities, source_need: sourceNeed, abstain,
    scores: scores ?? Object.fromEntries(capabilities.map((id, index) => [id, Math.max(0.5, 0.99 - index * 0.05)])) };
}

function classify(text) {
  const value = String(text).trim();
  const lower = value.toLowerCase();

  // High-precision structural/direct cases only. This is a control baseline,
  // not the intended production architecture.
  if (/https?:\/\/\S+/i.test(value) && /\b(read|open|summari[sz]e|tell me|main finding)\b/i.test(value))
    return result("web.read", { sourceNeed: "live" });
  if (/\b(search (?:the )?web|search online|check online|look (?:it )?up online|browse|find (?:it )?online)\b/i.test(value))
    return result("web.search", { sourceNeed: "live" });

  if (/\b(what time is it|current time|what(?:'s| is) the time|time in [A-Za-z])\b/i.test(value) && !/\bif it is\b/i.test(value))
    return result("time.current", { sourceNeed: "live" });
  if (/\b(convert|what time is that|time would that be)\b/i.test(value) && /\b(?:UTC|GMT|time|AM|PM|\d{1,2}:\d{2})\b/i.test(value))
    return result("time.convert", { sourceNeed: "stable" });

  if (new RegExp(`\\b${currencyCode}\\b[\\s\\S]{0,35}\\b(?:in|to|into|worth|rate|exchange)\\b[\\s\\S]{0,25}\\b${currencyCode}\\b`, "i").test(value) ||
      new RegExp(`\\b(?:rate|exchange|worth|convert)\\b[\\s\\S]{0,30}\\b${currencyCode}\\b[\\s\\S]{0,20}\\b${currencyCode}\\b`, "i").test(value))
    return result("currency.exchange", { sourceNeed: "live" });

  if (/\b(weather|temperature|raining|rain|umbrella|dry|hot|cold)\b/i.test(value) &&
      /\b(now|right now|currently|outside|today)\b/i.test(value) && !/\b(explain|why|what does|how does)\b/i.test(value))
    return result("weather.current", { sourceNeed: "live" });
  if (/\b(weather|temperature|rain|umbrella|dry|hot|cold|windows open)\b/i.test(value) &&
      /\b(tomorrow|tonight|evening|morning|afternoon|later|will|likely|forecast|Saturday|Sunday|Monday|Tuesday|Wednesday|Thursday|Friday)\b/i.test(value) && !/\b(explain|why|what does|how does)\b/i.test(value))
    return result("weather.forecast", { sourceNeed: "live" });

  if (/\b(score|win|won|result)\b/i.test(value) && /\b(match|game|Arsenal|Lakers|Barcelona|Liverpool|Warriors)\b/i.test(value) && !/\b(19\d\d|20(?:0\d|1\d))\b/.test(value))
    return result("sports.score", { sourceNeed: "live" });
  if (/\b(play next|next game|fixtures?|schedule)\b/i.test(value))
    return result("sports.schedule", { sourceNeed: "live" });

  if (/\b(trading at|stock price|worth now|current .* price|up today)\b/i.test(value) && /\b(Bitcoin|BTC|ETH|NVIDIA|stock|S&P|crypto)\b/i.test(value))
    return result("market.quote", { sourceNeed: "live" });

  if (/\b(what did i|earlier|last time|yesterday|we chose|we decided|we used|remember we)\b/i.test(value))
    return result("memory.retrieve", { sourceNeed: "internal" });

  if (/\b(volume|muted|cloud connection|device status)\b/i.test(value) && /\b(current|what|is|right now|up)\b/i.test(value) && !/\b(set|turn|reduce|increase|reset)\b/i.test(value))
    return result("device.state", { sourceNeed: "internal" });
  if (/\b(set|turn|reduce|increase|reset|mute|unmute)\b/i.test(value) && /\b(volume|voice mode|device|speaker)\b/i.test(value))
    return result("device.control", { sourceNeed: "internal" });

  if (/\b(CSI|sensor|RF signal|motion signal|motion sensor)\b/i.test(value) && /\b(reading|detecting|current|right now|how strong)\b/i.test(value) && !/\b(explain|how does|why)\b/i.test(value))
    return result("sensor.read", { sourceNeed: "internal" });
  if (/\b(camera|Newo2|can you see|look through)\b/i.test(value) && /\b(look|see|check|describe|holding|standing|desk)\b/i.test(value) && !/\b(explain|why|how does)\b/i.test(value))
    return result("camera.inspect", { sourceNeed: "internal" });

  if (new RegExp(`\\b(?:convert|turn|how many|what is)\\b[\\s\\S]{0,25}\\b${unit}\\b[\\s\\S]{0,20}\\b(?:to|in|into)\\b[\\s\\S]{0,20}\\b${unit}\\b`, "i").test(value))
    return result("unit.convert", { sourceNeed: "stable" });
  if (/\b(calculate|percent of|square root|split .* equally|times|add)\b/i.test(value) || /\d\s*[+*/%-]\s*\d/.test(value))
    return result("calculator", { sourceNeed: "stable" });

  // Open-world freshness is deliberately last so specialized sources win.
  if (/\b(latest|newest|released yet|release notes|current sources|today's .* news)\b/i.test(value))
    return result("web.search", { sourceNeed: "live" });

  // Very short unresolved references are safer to abstain than to guess.
  if (lower.split(/\s+/).length <= 5 && /\b(it|they|that|rate|outside)\b/i.test(value))
    return result(null, { capabilities: [], sourceNeed: "unknown", abstain: true, scores: {} });

  return result("knowledge", { sourceNeed: "stable" });
}

for (const row of rows) {
  const started = performance.now();
  const prediction = classify(row.text);
  const latency = performance.now() - started;
  console.log(JSON.stringify({ id: row.id, ...prediction, latency_ms: latency }));
}
