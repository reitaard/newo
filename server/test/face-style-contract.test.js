import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const serverRoot = new URL("../", import.meta.url);
const root = new URL("../../", import.meta.url);

test("Telegram face styles stay unique, enum-backed, and firmware-reachable", () => {
  const server = readFileSync(new URL("src/index.js", serverRoot), "utf8");
  const firmware = readFileSync(new URL("Newo/newo_cloud.cpp", root), "utf8");
  const header = readFileSync(new URL("Newo/newo_display.h", root), "utf8");

  const styles = [...server.match(/const FACE_STYLES = \[([^\]]+)\]/)[1].matchAll(/"([a-z_]+)"/g)]
    .map((match) => match[1]);
  assert.ok(styles.length > 0);
  assert.equal(new Set(styles).size, styles.length);

  for (const required of ["default", "closed", "detached", "sleeping", "skeptical"]) {
    assert.ok(styles.includes(required), `missing required face style: ${required}`);
  }

  for (const style of styles) {
    assert.match(firmware, new RegExp(`strcmp\\(mode, "${style}"\\)`));
  }

  const enums = header.match(/enum class NewoFaceStyle[^\{]*\{([^}]*)\}/)[1]
    .split(",")
    .map((value) => value.trim())
    .filter(Boolean);
  assert.equal(enums.length, styles.length);

  assert.match(server, /for \(const style of FACE_STYLES\) bot\.command\(`face_\$\{style\}`/);
});
