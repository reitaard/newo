import assert from "node:assert/strict";
import test from "node:test";
import { createStructuredCapabilityRuntime, structuredCapabilityContext } from "../src/assistant-capabilities.js";

const ok = (value) => new Response(JSON.stringify(value), { status: 200, headers: { "content-type": "application/json" } });

test("local calculator and unit conversion are deterministic and provider-free", async () => {
  let fetches = 0;
  const runtime = createStructuredCapabilityRuntime({ fetchImpl: async () => { fetches += 1; throw new Error("unexpected"); } });
  const calculation = await runtime.invoke("calculator", "Calculate (12 + 3) times 4");
  assert.equal(calculation.kind, "result");
  assert.equal(calculation.result.value, 60);
  const conversion = await runtime.invoke("unit.convert", "Convert 10 km to miles");
  assert.equal(conversion.kind, "result");
  assert.equal(conversion.result.result, 6.21371192);
  assert.equal(fetches, 0);
});

test("Open-Meteo geocoding and weather produce compact structured evidence", async () => {
  const urls = [];
  const runtime = createStructuredCapabilityRuntime({ fetchImpl: async (url) => {
    urls.push(String(url));
    if (String(url).includes("geocoding-api")) return ok({ results: [{ name: "Phnom Penh", country: "Cambodia",
      latitude: 11.56, longitude: 104.92, timezone: "Asia/Phnom_Penh" }] });
    return ok({ current: { time: "2026-09-14T10:00", temperature_2m: 31, apparent_temperature: 36,
      precipitation: 0, weather_code: 2, wind_speed_10m: 8 } });
  } });
  const result = await runtime.invoke("weather.current", "What's the weather in Phnom Penh?");
  assert.equal(result.kind, "result");
  assert.equal(result.provider, "open_meteo");
  assert.equal(result.result.condition, "partly cloudy");
  assert.equal(urls.length, 2);
  assert.match(structuredCapabilityContext(result), /Authoritative structured weather\.current data/);
});

test("Frankfurter v2 performs reference conversion while keyed providers fail open when absent", async () => {
  let fetches = 0;
  const runtime = createStructuredCapabilityRuntime({ fetchImpl: async (url) => {
    fetches += 1;
    assert.match(String(url), /\/v2\/rate\/USD\/EUR$/); return ok({ date: "2026-09-14", base: "USD", quote: "EUR", rate: 0.85 });
  } });
  const fx = await runtime.invoke("currency.exchange", "Convert 20 dollars to euros");
  assert.equal(fx.kind, "result");
  assert.equal(fx.result.converted, 17);
  const cachedFx = await runtime.invoke("currency.exchange", "Convert 40 dollars to euros");
  assert.equal(cachedFx.result.converted, 34);
  assert.equal(fetches, 1);
  assert.deepEqual(await runtime.invoke("market.quote", "What's AAPL trading at?"),
    { kind: "provider_unavailable", tool: "market_quote", reason: "missing_api_key" });
  const sports = await runtime.invoke("sports.score", "Arsenal score");
  assert.equal(sports.kind, "provider_unavailable");
  assert.equal(sports.reason, "missing_api_key");
});

test("missing slots and provider failures return fallback signals without fabricated data", async () => {
  const missing = createStructuredCapabilityRuntime();
  assert.equal((await missing.invoke("weather.forecast", "Will it rain?")).kind, "missing_slots");
  const failed = createStructuredCapabilityRuntime({ fetchImpl: async () => new Response("down", { status: 503 }) });
  const result = await failed.invoke("weather.current", "Weather in Berlin");
  assert.equal(result.kind, "provider_error");
  assert.equal(result.reason, "structured_provider_error");
  assert.equal(result.result, undefined);
});
