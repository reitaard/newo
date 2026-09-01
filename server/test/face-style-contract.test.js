import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const serverRoot = new URL("../", import.meta.url);
const repositoryRoot = new URL("../../", import.meta.url);
test("Telegram face styles remain reachable in firmware", () => {
  const server = readFileSync(new URL("src/index.js", serverRoot), "utf8");
  const firmware = readFileSync(new URL("Newo/newo_cloud.cpp", repositoryRoot), "utf8");
  const match = server.match(/const FACE_STYLES = \[([^\]]+)\]/);
  assert.ok(match);
  for (const style of match[1].matchAll(/"([a-z_]+)"/g)) {
    assert.match(firmware, new RegExp(`strcmp\\(mode, "${style[1]}"\\)`));
  }
});
