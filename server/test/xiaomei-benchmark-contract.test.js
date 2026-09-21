import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const fixtureUrl = new URL("../benchmarks/xiaomei/controller-v1.json", import.meta.url);
const scriptUrl = new URL("../scripts/benchmark-xiaomei-controller.mjs", import.meta.url);
const shootoutUrl = new URL("../scripts/benchmark-xiaomei-shootout.mjs", import.meta.url);
const candidateUrl = new URL("../benchmarks/xiaomei/candidate.json", import.meta.url);
const candidatesUrl = new URL("../benchmarks/xiaomei/candidates.json", import.meta.url);
const packageUrl = new URL("../package.json", import.meta.url);

const allowedRoutes = new Set([
  "chat",
  "teach",
  "translate_once",
  "interpreter_start",
  "interpreter_translate",
  "interpreter_meta",
  "interpreter_stop",
]);

test("Xiaomei benchmark fixture covers the controller modes without duplicate cases", async () => {
  const fixture = JSON.parse(await readFile(fixtureUrl, "utf8"));
  assert.equal(fixture.version, 1);
  assert.ok(Array.isArray(fixture.cases));
  assert.ok(fixture.cases.length >= 16);

  const ids = new Set();
  const routesSeen = new Set();
  for (const caseDef of fixture.cases) {
    assert.equal(typeof caseDef.id, "string");
    assert.ok(!ids.has(caseDef.id), `duplicate benchmark id: ${caseDef.id}`);
    ids.add(caseDef.id);
    assert.ok(allowedRoutes.has(caseDef.expected_route), `unsupported route: ${caseDef.expected_route}`);
    routesSeen.add(caseDef.expected_route);
    assert.ok(Array.isArray(caseDef.messages) && caseDef.messages.length > 0, `${caseDef.id} has no messages`);
  }

  for (const route of allowedRoutes) assert.ok(routesSeen.has(route), `fixture does not cover ${route}`);
});

test("Xiaomei single-model benchmark is fixed to 4096 context and writes durable reports", async () => {
  const source = await readFile(scriptUrl, "utf8");
  assert.match(source, /const CONTEXT_SIZE = 4096;/);
  assert.match(source, /LATEST\.json/);
  assert.match(source, /LATEST\.md/);
  assert.match(source, /\/api\/ps/);
  assert.match(source, /first_content_ms/);
  assert.match(source, /decode_tps/);
});

test("single-candidate diagnostic config remains isolated from production profiles", async () => {
  const candidate = JSON.parse(await readFile(candidateUrl, "utf8"));
  assert.equal(candidate.label, null);
  assert.equal(candidate.base_url, null);
  assert.equal(candidate.model, null);
});

test("official Xiaomei shootout is pinned to the downloaded VPS models", async () => {
  const config = JSON.parse(await readFile(candidatesUrl, "utf8"));
  assert.equal(config.version, 1);
  assert.equal(config.endpoint, "http://100.110.136.15:11435/api/chat");
  assert.deepEqual(config.candidates.map((candidate) => candidate.model), [
    "newo-minicpm5:latest",
    "newo-qwen35-2b:latest",
    "newo-spark17:latest",
  ]);
  assert.deepEqual(config.candidates.map((candidate) => candidate.label), [
    "MiniCPM5-2B",
    "Qwen3.5-2B",
    "Spark-X2.5-1.7B",
  ]);
});

test("shootout runs candidates sequentially and writes a combined report", async () => {
  const source = await readFile(shootoutUrl, "utf8");
  assert.match(source, /spawnSync/);
  assert.match(source, /SHOOTOUT-LATEST\.json/);
  assert.match(source, /SHOOTOUT-LATEST\.md/);
  assert.match(source, /keep_alive: 0/);
});

test("package exposes one-command Xiaomei shootout and a single-model diagnostic", async () => {
  const pkg = JSON.parse(await readFile(packageUrl, "utf8"));
  assert.equal(pkg.scripts["xiaomei:benchmark"], "node scripts/benchmark-xiaomei-shootout.mjs");
  assert.equal(pkg.scripts["xiaomei:benchmark:single"], "node scripts/benchmark-xiaomei-controller.mjs");
  assert.match(pkg.scripts.check, /benchmark-xiaomei-controller\.mjs/);
  assert.match(pkg.scripts.check, /benchmark-xiaomei-shootout\.mjs/);
});
