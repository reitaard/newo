import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const serverRoot = new URL("../", import.meta.url), root = new URL("../../", import.meta.url);
test("exactly twenty-two Telegram face styles are firmware-reachable", () => {
  const server = readFileSync(new URL("src/index.js", serverRoot), "utf8");
  const firmware = readFileSync(new URL("Newo/newo_cloud.cpp", root), "utf8");
  const header = readFileSync(new URL("Newo/newo_display.h", root), "utf8");
  const styles = [...server.match(/const FACE_STYLES = \[([^\]]+)\]/)[1].matchAll(/"([a-z_]+)"/g)].map(x => x[1]);
  assert.equal(styles.length, 22);
  for (const style of styles) assert.match(firmware, new RegExp(`strcmp\\(mode, "${style}"\\)`));
  const enums = header.match(/enum class NewoFaceStyle[^\{]*\{([^}]*)\}/)[1].split(",").map(x => x.trim()).filter(Boolean);
  assert.equal(enums.length, 22);
  for (const style of styles) assert.match(server, /for \(const style of FACE_STYLES\) bot\.command\(`face_\$\{style\}`/);
});
