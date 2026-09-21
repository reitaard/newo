import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const fixtureUrl = new URL("../benchmarks/xiaomei/controller-v1.json", import.meta.url);
const scriptUrl = new URL("../scripts/benchmark-xiaomei-controller.mjs", import.meta.url);
const shootoutUrl = new URL("../scripts/benchmark-xiaomei-shootout.mjs", import.meta.url);
const candidateUrl = new URL("../benchmarks/xiaomei/candidate.json", import.meta.url);
const candidatesUrl = new URL("../benchmarks/xiaomei/candidates.json", import.meta.url);
const qualityFixtureUrl = new URL("../benchmarks/xiaomei/quality-v1.json", import.meta.url);
const qualityCandidatesUrl = new URL("../benchmarks/xiaomei/quality-candidates.json", import.meta.url);
const qualityScriptUrl = new URL("../scripts/benchmark-xiaomei-quality.mjs", import.meta.url);
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

test("Xiaomei single-model benchmark is fixed to 4096 context, supports schema/think modes, and writes durable reports", async () => {
  const source = await readFile(scriptUrl, "utf8");
  assert.match(source, /const CONTEXT_SIZE = 4096;/);
  assert.match(source, /XIAOMEI_BENCH_OUTPUT_MODE/);
  assert.match(source, /XIAOMEI_BENCH_THINK/);
  assert.match(source, /CONTROLLER_SCHEMA/);
  assert.match(source, /body\.format = CONTROLLER_SCHEMA/);
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

test("official Xiaomei schema shootout is pinned to the downloaded VPS finalists", async () => {
  const config = JSON.parse(await readFile(candidatesUrl, "utf8"));
  assert.equal(config.version, 2);
  assert.equal(config.endpoint, "http://100.110.136.15:11435/api/chat");
  assert.deepEqual(config.candidates.map((candidate) => candidate.model), [
    "newo-qwen35-2b:latest",
    "newo-qwen38-2b:latest",
    "newo-gemma-e2b:latest",
    "newo-minicpm5:latest",
    "newo-spark17:latest",
  ]);
  assert.deepEqual(config.candidates.map((candidate) => candidate.label), [
    "Qwen3.5-2B",
    "Qwen3.8-2B",
    "Gemma-4-E2B",
    "MiniCPM5-2B",
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

test("Xiaomei quality fixture targets Chinese correctness after routing is known", async () => {
  const fixture = JSON.parse(await readFile(qualityFixtureUrl, "utf8"));
  assert.equal(fixture.version, 1);
  assert.ok(Array.isArray(fixture.cases));
  assert.ok(fixture.cases.length >= 10);
  const ids = new Set(fixture.cases.map((caseDef) => caseDef.id));
  assert.equal(ids.size, fixture.cases.length);
  for (const required of ["qingwen-pronunciation", "preserve-tomorrow-time", "cai-emphatic-denial", "code-switch-cleanup"]) {
    assert.ok(ids.has(required), `quality fixture missing ${required}`);
  }
});

test("Xiaomei quality shootout is limited to the three FAST finalists", async () => {
  const config = JSON.parse(await readFile(qualityCandidatesUrl, "utf8"));
  assert.equal(config.endpoint, "http://100.110.136.15:11435/api/chat");
  assert.deepEqual(config.candidates.map((candidate) => candidate.model), [
    "newo-minicpm5:latest",
    "newo-gemma-e2b:latest",
    "newo-qwen35-2b:latest",
  ]);
});

test("Xiaomei quality benchmark measures direct responses with bounded THINK", async () => {
  const source = await readFile(qualityScriptUrl, "utf8");
  assert.match(source, /const CONTEXT_SIZE = 4096;/);
  assert.match(source, /XIAOMEI_QUALITY_THINK/);
  assert.match(source, /XIAOMEI_QUALITY_NUM_PREDICT/);
  assert.match(source, /"768"/);
  assert.match(source, /"15000"/);
  assert.match(source, /do not classify it or output routing JSON/i);
  assert.match(source, /QUALITY-SHOOTOUT-LATEST\.json/);
  assert.match(source, /QUALITY-SHOOTOUT-LATEST\.md/);
  assert.match(source, /thinking_chars/);
  assert.match(source, /anchor_score_pct/);
});

test("package exposes Xiaomei controller and response-quality commands", async () => {
  const pkg = JSON.parse(await readFile(packageUrl, "utf8"));
  assert.equal(pkg.scripts["xiaomei:benchmark"], "node scripts/benchmark-xiaomei-shootout.mjs");
  assert.equal(pkg.scripts["xiaomei:benchmark:single"], "node scripts/benchmark-xiaomei-controller.mjs");
  assert.equal(pkg.scripts["xiaomei:quality"], "node scripts/benchmark-xiaomei-quality.mjs");
  assert.match(pkg.scripts.check, /benchmark-xiaomei-controller\.mjs/);
  assert.match(pkg.scripts.check, /benchmark-xiaomei-shootout\.mjs/);
  assert.match(pkg.scripts.check, /benchmark-xiaomei-quality\.mjs/);
});
