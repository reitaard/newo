const CURRENCY_NAMES = Object.freeze({
  dollar: "USD", dollars: "USD", usd: "USD", euro: "EUR", euros: "EUR", eur: "EUR",
  pound: "GBP", pounds: "GBP", gbp: "GBP", yen: "JPY", jpy: "JPY", baht: "THB", thb: "THB",
  riel: "KHR", khr: "KHR", yuan: "CNY", cny: "CNY", rupee: "INR", rupees: "INR", inr: "INR",
});

const UNIT_TABLE = Object.freeze({
  length: Object.freeze({ mm: 0.001, millimeter: 0.001, millimeters: 0.001, cm: 0.01, centimeter: 0.01,
    centimeters: 0.01, m: 1, meter: 1, meters: 1, km: 1000, kilometer: 1000, kilometers: 1000,
    in: 0.0254, inch: 0.0254, inches: 0.0254, ft: 0.3048, foot: 0.3048, feet: 0.3048,
    yd: 0.9144, yard: 0.9144, yards: 0.9144, mi: 1609.344, mile: 1609.344, miles: 1609.344 }),
  mass: Object.freeze({ mg: 0.001, milligram: 0.001, milligrams: 0.001, g: 1, gram: 1, grams: 1,
    kg: 1000, kilogram: 1000, kilograms: 1000, oz: 28.349523125, ounce: 28.349523125,
    ounces: 28.349523125, lb: 453.59237, pound: 453.59237, pounds: 453.59237 }),
});

const WEATHER_CODES = Object.freeze({ 0: "clear", 1: "mostly clear", 2: "partly cloudy", 3: "overcast",
  45: "foggy", 48: "foggy", 51: "light drizzle", 53: "drizzle", 55: "heavy drizzle", 61: "light rain",
  63: "rain", 65: "heavy rain", 71: "light snow", 73: "snow", 75: "heavy snow", 80: "rain showers",
  81: "rain showers", 82: "heavy rain showers", 95: "thunderstorms", 96: "thunderstorms with hail",
  99: "thunderstorms with hail" });

const FAST_CAPABILITIES = new Set(["calculator", "unit.convert", "time.current", "time.convert", "currency.exchange",
  "weather.current", "weather.forecast", "sports.score", "sports.schedule", "market.quote"]);

function capabilityError(code, message = code) { const error = new Error(message); error.code = code; return error; }
function round(value, places = 4) { const scale = 10 ** places; return Math.round(value * scale) / scale; }
function words(value) { return String(value).toLowerCase().match(/[a-z]{2,}|[A-Z]{2,5}|\d+(?:\.\d+)?/g) ?? []; }

function arithmetic(text) {
  let source = String(text).toLowerCase().replace(/\b(?:what is|calculate|compute|please|equals?|answer)\b/g, " ")
    .replace(/multiplied by|times/g, "*").replace(/divided by|over/g, "/").replace(/plus/g, "+").replace(/minus/g, "-")
    .replace(/to the power of/g, "^").replace(/[^0-9eE+\-*/%^().\s]/g, "").trim();
  if (!source || source.length > 120) return null;
  let index = 0;
  const ws = () => { while (/\s/.test(source[index] ?? "")) index += 1; };
  const number = () => { ws(); const match = /^(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?/i.exec(source.slice(index));
    if (!match) throw capabilityError("invalid_expression"); index += match[0].length; return Number(match[0]); };
  const primary = () => { ws(); if (source[index] === "(") { index += 1; const value = expression(); ws();
    if (source[index++] !== ")") throw capabilityError("invalid_expression"); return value; }
    if (source[index] === "+" || source[index] === "-") return source[index++] === "-" ? -primary() : primary(); return number(); };
  const power = () => { let value = primary(); ws(); if (source[index] === "^") { index += 1; value **= power(); } return value; };
  const product = () => { let value = power(); while (true) { ws(); const op = source[index]; if (!["*", "/", "%"].includes(op)) break;
    index += 1; const right = power(); if ((op === "/" || op === "%") && right === 0) throw capabilityError("division_by_zero");
    value = op === "*" ? value * right : op === "/" ? value / right : value % right; } return value; };
  const expression = () => { let value = product(); while (true) { ws(); const op = source[index]; if (!["+", "-"].includes(op)) break;
    index += 1; const right = product(); value = op === "+" ? value + right : value - right; } return value; };
  try { const value = expression(); ws(); return index === source.length && Number.isFinite(value) ? { expression: source, value } : null; } catch { return null; }
}

function extractUnit(text) {
  const match = String(text).toLowerCase().match(/(-?\d+(?:\.\d+)?)\s*([a-z]+)\s+(?:in|into|to)\s+([a-z]+)/);
  if (!match) return null;
  for (const [dimension, units] of Object.entries(UNIT_TABLE)) if (units[match[2]] && units[match[3]]) {
    const value = Number(match[1]); return { dimension, value, from_unit: match[2], to_unit: match[3],
      result: value * units[match[2]] / units[match[3]] };
  }
  if (["c", "celsius", "f", "fahrenheit", "k", "kelvin"].includes(match[2]) &&
      ["c", "celsius", "f", "fahrenheit", "k", "kelvin"].includes(match[3])) {
    const celsius = match[2].startsWith("f") ? (Number(match[1]) - 32) * 5 / 9 : match[2].startsWith("k") ? Number(match[1]) - 273.15 : Number(match[1]);
    const result = match[3].startsWith("f") ? celsius * 9 / 5 + 32 : match[3].startsWith("k") ? celsius + 273.15 : celsius;
    return { dimension: "temperature", value: Number(match[1]), from_unit: match[2], to_unit: match[3], result };
  }
  return null;
}

function extractCurrencies(text) {
  const tokens = words(text); const found = [];
  for (const token of tokens) { const code = CURRENCY_NAMES[token.toLowerCase()] ?? (/^[A-Z]{3}$/.test(token) ? token : null);
    if (code && !found.includes(code)) found.push(code); }
  const amount = Number((String(text).match(/\b\d+(?:\.\d+)?\b/) ?? [1])[0]);
  return found.length >= 2 ? { amount, base: found[0], quote: found[1] } : null;
}

function extractLocation(text) {
  const value = String(text).trim().replace(/[?.!]+$/g, "");
  const match = value.match(/\b(?:in|for|at|near)\s+([\p{L}][\p{L}\s,'-]{1,80})$/iu);
  if (!match) return null;
  return match[1].replace(/\b(?:today|tonight|tomorrow|right now|currently|this week|next week)$/i, "").trim();
}

function extractMarketSymbol(text) {
  const explicit = String(text).match(/\b[A-Z]{1,6}(?:\/[A-Z]{3,6})?\b/g)?.find((token) => !["WHAT", "PRICE", "CURRENT", "TODAY"].includes(token));
  return explicit ?? null;
}

function extractTimeConversion(text) {
  const match = String(text).trim().match(/\b(\d{1,2})(?::(\d{2}))?\s*(am|pm)?\s+(?:in|from)\s+(.+?)\s+(?:to|into)\s+(.+?)[?.!]*$/i);
  if (!match) return null;
  let hour = Number(match[1]); const minute = Number(match[2] ?? 0); const meridiem = match[3]?.toLowerCase();
  if (minute > 59 || hour > (meridiem ? 12 : 23) || hour < 0) return null;
  if (meridiem) hour = hour % 12 + (meridiem === "pm" ? 12 : 0);
  return { hour, minute, source: match[4].trim(), target: match[5].trim() };
}

function zonedDateToUtc({ year, month, day, hour, minute }, timeZone) {
  let instant = Date.UTC(year, month - 1, day, hour, minute);
  const formatter = new Intl.DateTimeFormat("en-CA", { timeZone, year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", hourCycle: "h23" });
  for (let attempt = 0; attempt < 2; attempt += 1) {
    const parts = Object.fromEntries(formatter.formatToParts(new Date(instant)).map((part) => [part.type, part.value]));
    const represented = Date.UTC(Number(parts.year), Number(parts.month) - 1, Number(parts.day), Number(parts.hour), Number(parts.minute));
    instant += Date.UTC(year, month - 1, day, hour, minute) - represented;
  }
  return new Date(instant);
}

function toolFor(capability) {
  return ({ calculator: "calculate", "unit.convert": "unit_convert", "time.current": "time_current", "time.convert": "time_convert",
    "currency.exchange": "currency_reference", "weather.current": "weather_current", "weather.forecast": "weather_forecast",
    "market.quote": "market_quote", "sports.score": "sports_lookup", "sports.schedule": "sports_lookup" })[capability] ?? null;
}

async function fetchJson(fetchImpl, url, { timeoutMs, signal, headers = {} }) {
  const controller = new AbortController(); const timer = setTimeout(() => controller.abort(capabilityError("structured_provider_timeout")), timeoutMs);
  const abort = () => controller.abort(signal?.reason ?? capabilityError("assistant_cancelled")); signal?.addEventListener("abort", abort, { once: true });
  try { const response = await fetchImpl(url, { headers: { accept: "application/json", ...headers }, signal: controller.signal });
    const payload = await response.json().catch(() => null); if (!response.ok) throw capabilityError("structured_provider_error"); return payload;
  } catch (error) { if (controller.signal.aborted) throw controller.signal.reason; throw error?.code ? error : capabilityError("structured_provider_error"); }
  finally { clearTimeout(timer); signal?.removeEventListener("abort", abort); }
}

export function createStructuredCapabilityRuntime({ enabled = true, fetchImpl = fetch, timeoutMs = 3_000,
  timeZone = "Asia/Phnom_Penh", now = () => new Date(), twelveDataApiKey = "", apiSportsKey = "", logger = null } = {}) {
  async function geocode(location, signal) {
    const url = new URL("https://geocoding-api.open-meteo.com/v1/search"); url.searchParams.set("name", location);
    url.searchParams.set("count", "1"); url.searchParams.set("language", "en"); url.searchParams.set("format", "json");
    const payload = await fetchJson(fetchImpl, url, { timeoutMs, signal }); const place = payload?.results?.[0];
    if (!place) throw capabilityError("location_not_found");
    return { name: place.name, admin1: place.admin1 ?? null, country: place.country ?? null, latitude: place.latitude,
      longitude: place.longitude, timezone: place.timezone };
  }

  async function invoke(capability, text, { signal } = {}) {
    if (!enabled || !FAST_CAPABILITIES.has(capability)) return { kind: "not_applicable" };
    const startedAt = performance.now(); const tool = toolFor(capability); let provider = "local"; let slots = null; let result = null;
    try {
      if (capability === "calculator") { slots = arithmetic(text); if (!slots) return { kind: "missing_slots", tool, reason: "missing_slots" };
        result = { expression: slots.expression, value: round(slots.value, 8) }; }
      else if (capability === "unit.convert") { slots = extractUnit(text); if (!slots) return { kind: "missing_slots", tool, reason: "missing_slots" };
        result = { ...slots, result: round(slots.result, 8) }; }
      else if (capability === "time.current") {
        const location = extractLocation(text); const place = location ? await geocode(location, signal) : { name: null, timezone: timeZone };
        slots = { location, timezone: place.timezone }; result = { location: place.name, timezone: place.timezone,
          local_time: new Intl.DateTimeFormat("en-US", { timeZone: place.timezone, dateStyle: "full", timeStyle: "short" }).format(now()) };
      } else if (capability === "time.convert") { slots = extractTimeConversion(text); if (!slots) return { kind: "missing_slots", tool, reason: "missing_slots" };
        const sourcePlace = slots.source.includes("/") ? { name: slots.source, timezone: slots.source } : await geocode(slots.source, signal);
        const targetPlace = slots.target.includes("/") ? { name: slots.target, timezone: slots.target } : await geocode(slots.target, signal);
        const sourceDate = Object.fromEntries(new Intl.DateTimeFormat("en-CA", { timeZone: sourcePlace.timezone,
          year: "numeric", month: "2-digit", day: "2-digit" }).formatToParts(now()).map((part) => [part.type, part.value]));
        const instant = zonedDateToUtc({ year: Number(sourceDate.year), month: Number(sourceDate.month), day: Number(sourceDate.day),
          hour: slots.hour, minute: slots.minute }, sourcePlace.timezone);
        result = { source: sourcePlace.name, source_timezone: sourcePlace.timezone, target: targetPlace.name,
          target_timezone: targetPlace.timezone, converted_time: new Intl.DateTimeFormat("en-US", { timeZone: targetPlace.timezone,
            weekday: "long", hour: "numeric", minute: "2-digit", hour12: true }).format(instant) };
      } else if (capability === "currency.exchange") { slots = extractCurrencies(text); if (!slots) return { kind: "missing_slots", tool, reason: "missing_slots" };
        provider = "frankfurter_v2"; const payload = await fetchJson(fetchImpl,
          `https://api.frankfurter.dev/v2/rate/${encodeURIComponent(slots.base)}/${encodeURIComponent(slots.quote)}`, { timeoutMs, signal });
        if (!Number.isFinite(payload?.rate)) throw capabilityError("structured_provider_invalid");
        result = { ...slots, date: payload.date, rate: payload.rate, converted: round(slots.amount * payload.rate, 6), reference_rate: true };
      } else if (capability === "weather.current" || capability === "weather.forecast") { const location = extractLocation(text);
        if (!location) return { kind: "missing_slots", tool, reason: "missing_slots" }; slots = { location }; provider = "open_meteo"; const place = await geocode(location, signal);
        const url = new URL("https://api.open-meteo.com/v1/forecast"); url.searchParams.set("latitude", String(place.latitude));
        url.searchParams.set("longitude", String(place.longitude)); url.searchParams.set("timezone", place.timezone || "auto");
        if (capability === "weather.current") url.searchParams.set("current", "temperature_2m,apparent_temperature,precipitation,weather_code,wind_speed_10m");
        else { url.searchParams.set("daily", "weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"); url.searchParams.set("forecast_days", "7"); }
        const payload = await fetchJson(fetchImpl, url, { timeoutMs, signal });
        result = capability === "weather.current" ? { location: place, observed_at: payload.current?.time, temperature_c: payload.current?.temperature_2m,
          apparent_c: payload.current?.apparent_temperature, precipitation_mm: payload.current?.precipitation,
          wind_kmh: payload.current?.wind_speed_10m, condition: WEATHER_CODES[payload.current?.weather_code] ?? `code ${payload.current?.weather_code}` }
          : { location: place, timezone: payload.timezone, days: (payload.daily?.time ?? []).map((date, index) => ({ date,
            condition: WEATHER_CODES[payload.daily.weather_code[index]] ?? `code ${payload.daily.weather_code[index]}`,
            min_c: payload.daily.temperature_2m_min[index], max_c: payload.daily.temperature_2m_max[index],
            precipitation_probability_percent: payload.daily.precipitation_probability_max[index] })) };
      } else if (capability === "market.quote") { const symbol = extractMarketSymbol(text); if (!symbol) return { kind: "missing_slots", tool, reason: "missing_slots" };
        if (!twelveDataApiKey) return { kind: "provider_unavailable", tool, reason: "missing_api_key" }; slots = { symbol }; provider = "twelve_data";
        const url = new URL("https://api.twelvedata.com/quote"); url.searchParams.set("symbol", symbol); url.searchParams.set("apikey", twelveDataApiKey);
        const payload = await fetchJson(fetchImpl, url, { timeoutMs, signal }); if (payload?.status === "error") throw capabilityError("structured_provider_error");
        result = { symbol: payload.symbol ?? symbol, name: payload.name ?? null, exchange: payload.exchange ?? null, currency: payload.currency ?? null,
          price: Number(payload.close), change: Number(payload.change), percent_change: Number(payload.percent_change), timestamp: payload.datetime ?? null };
      } else if (capability === "sports.score" || capability === "sports.schedule") { const query = extractLocation(text) ?? String(text).replace(/[?.!]/g, "").trim();
        if (!apiSportsKey) return { kind: "provider_unavailable", tool, reason: "missing_api_key" }; slots = { query }; provider = "api_sports";
        const teamUrl = new URL("https://v3.football.api-sports.io/teams"); teamUrl.searchParams.set("search", query.slice(0, 40));
        const teams = await fetchJson(fetchImpl, teamUrl, { timeoutMs, signal, headers: { "x-apisports-key": apiSportsKey } });
        const team = teams?.response?.[0]?.team; if (!team?.id) throw capabilityError("sports_team_not_found");
        const fixturesUrl = new URL("https://v3.football.api-sports.io/fixtures"); fixturesUrl.searchParams.set("team", String(team.id));
        fixturesUrl.searchParams.set(capability === "sports.score" ? "last" : "next", "3"); fixturesUrl.searchParams.set("timezone", "UTC");
        const fixtures = await fetchJson(fetchImpl, fixturesUrl, { timeoutMs, signal, headers: { "x-apisports-key": apiSportsKey } });
        result = { team: team.name, fixtures: (fixtures?.response ?? []).slice(0, 3).map((entry) => ({ at: entry.fixture?.date,
          status: entry.fixture?.status?.short, home: entry.teams?.home?.name, away: entry.teams?.away?.name,
          home_goals: entry.goals?.home, away_goals: entry.goals?.away })) };
      }
      const elapsedMs = Math.round(performance.now() - startedAt); logger?.info({ capability, structured_tool: tool, structured_provider: provider,
        structured_provider_ms: elapsedMs }, "Assistant structured capability completed");
      return { kind: "result", capability, tool, provider, slots, result, elapsed_ms: elapsedMs };
    } catch (error) { const elapsedMs = Math.round(performance.now() - startedAt); const reason = error?.code ?? "structured_provider_error";
      logger?.warn({ capability, structured_tool: tool, structured_provider: provider, structured_provider_ms: elapsedMs, error_code: reason },
        "Assistant structured capability fell back"); return { kind: "provider_error", capability, tool, provider, reason, elapsed_ms: elapsedMs }; }
  }
  return { enabled, supports: (capability) => enabled && FAST_CAPABILITIES.has(capability), invoke };
}

export function structuredCapabilityContext(invocation) {
  if (invocation?.kind !== "result") return null;
  return `Authoritative structured ${invocation.capability} data from ${invocation.provider}: ${JSON.stringify(invocation.result)}. Use it to answer the user directly and concisely. Do not call another tool for the same fact.`;
}
