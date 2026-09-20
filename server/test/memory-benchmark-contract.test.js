import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const datasetUrl = new URL("../benchmarks/memory/newo-gold-v1.json", import.meta.url);

test("memory benchmark gold dataset has unique valid cases", async () => {
  const dataset = JSON.parse(await readFile(datasetUrl, "utf8"));
  assert.ok(Array.isArray(dataset.cases));
  assert.ok(dataset.cases.length >= 20);
  const ids = new Set();
  let positive = 0;
  let negative = 0;
  for (const item of dataset.cases) {
    assert.equal(typeof item.id, "string");
    assert.ok(item.id.length > 0);
    assert.ok(!ids.has(item.id), `duplicate case id: ${item.id}`);
    ids.add(item.id);
    assert.equal(typeof item.input, "string");
    assert.ok(Array.isArray(item.expected));
    if (item.expected.length) positive += 1; else negative += 1;
    for (const expected of item.expected) {
      assert.ok(Array.isArray(expected.terms) && expected.terms.length > 0);
      assert.equal(typeof expected.type, "string");
    }
  }
  assert.ok(positive > 0);
  assert.ok(negative >= 8, "negative cases are required to measure false-memory behavior");
});
