import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const serverRoot = new URL("../", import.meta.url);

test("Telegram menu exposes core controls, semantic faces, and composed reactions", () => {
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
    "face_sleeping", "face_unimpressed", "face_skeptical",
    "effect_none", "effect_question", "effect_exclamation", "effect_surprise", "effect_ellipsis", "effect_sweat", "effect_zzz",
    "caption_none", "caption_huh", "caption_woah", "caption_hmm", "caption_hey", "caption_wtf", "caption_tsk",
    "reaction", "reaction_none", "reaction_huh", "reaction_woah", "reaction_hmm", "reaction_hey", "reaction_wtf", "reaction_tsk",
    "eco", "clock", "voice", "speaker", "ping",
  ]) {
    assert.ok(commands.some(({ command }) => command === required), `missing Telegram menu command: ${required}`);
  }

  for (const command of commands.map(({ command }) => command).filter((value) => value.startsWith("face_"))) {
    assert.ok(faceStyles.includes(command.slice("face_".length)), `menu face is not firmware-backed: ${command}`);
  }

  const reactionMappings = [
    ["none", "default", "none", "none"],
    ["huh", "curious", "question", "huh"],
    ["woah", "surprised", "surprise", "woah"],
    ["hmm", "confused", "question", "hmm"],
    ["hey", "happy", "exclamation", "hey"],
    ["wtf", "surprised", "surprise", "wtf"],
    ["tsk", "unimpressed", "ellipsis", "tsk"],
  ];
  for (const [reaction, face, effect, caption] of reactionMappings) {
    assert.match(server, new RegExp(`${reaction}: \\{ face: "${face}", effect: "${effect}", caption: "${caption}" \\}`));
  }

  assert.match(server, /for \(const style of FACE_STYLES\) bot\.command\(`face_\$\{style\}`/);
  assert.match(server, /bot\.command\(\["face", "f"\], handleFaceCommand\)/);
  assert.match(server, /function secondaryEffectCommand\(effect\) \{ return `\/effect_\$\{effect\}`; \}/);
  assert.match(server, /function faceCaptionCommand\(caption\) \{ return `\/caption_\$\{caption\}`; \}/);
  assert.match(server, /for \(const effect of SECONDARY_EFFECTS\) bot\.command\(`effect_\$\{effect\}`/);
  assert.match(server, /for \(const caption of FACE_CAPTIONS\) bot\.command\(`caption_\$\{caption\}`/);
  assert.match(server, /bot\.command\(\["reaction", "rx"\], handleReactionCommand\)/);
  assert.match(server, /for \(const reaction of REACTION_NAMES\) bot\.command\(`reaction_\$\{reaction\}`/);
  assert.match(server, /const steps = \[[\s\S]*mode: preset\.face[\s\S]*mode: "effect"[\s\S]*mode: "caption"/);
  assert.match(server, /bot\.api\.setMyCommands\(TELEGRAM_COMMANDS\)/);
});
