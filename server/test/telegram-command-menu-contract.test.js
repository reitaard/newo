import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const serverRoot = new URL("../", import.meta.url);

test("Telegram menu exposes core controls and semantic display faces", () => {
  const server = readFileSync(new URL("src/index.js", serverRoot), "utf8");

  const faceStyles = [...server.match(/const FACE_STYLES = \[([^\]]+)\]/)[1].matchAll(/"([a-z_]+)"/g)]
    .map((match) => match[1]);
  const menuBlock = server.match(/const TELEGRAM_COMMANDS = \[([\s\S]*?)\n\];/);
  assert.ok(menuBlock, "Telegram command menu must exist");

  const commands = [...menuBlock[1].matchAll(/\{ command: "([a-z0-9_]+)", description: "([^"]+)" \}/g)]
    .map((match) => ({ command: match[1], description: match[2] }));
  assert.ok(commands.length > 0, "Telegram command menu must not be empty");
  assert.equal(new Set(commands.map(({ command }) => command)).size, commands.length, "Telegram menu commands must be unique");

  for (const { command, description } of commands) {
    assert.match(command, /^[a-z0-9_]{1,32}$/);
    assert.ok(description.length > 0 && description.length <= 256);
  }

  for (const required of [
    "status", "health", "face", "face_default", "face_closed", "face_detached",
    "face_sleeping", "face_skeptical", "eco", "clock", "voice", "speaker", "ping",
  ]) {
    assert.ok(commands.some(({ command }) => command === required), `missing Telegram menu command: ${required}`);
  }

  for (const command of commands.map(({ command }) => command).filter((value) => value.startsWith("face_"))) {
    assert.ok(faceStyles.includes(command.slice("face_".length)), `menu face is not firmware-backed: ${command}`);
  }

  assert.match(server, /for \(const style of FACE_STYLES\) bot\.command\(`face_\$\{style\}`/);
  assert.match(server, /bot\.command\(\["face", "f"\], handleFaceCommand\)/);
  assert.match(server, /bot\.api\.setMyCommands\(TELEGRAM_COMMANDS\)/);
});
