import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { resolveSherpaLmConfig } from "../src/sherpa-lm-config.js";
import { applySherpaDecoderConfig } from "../src/voice.js";

const hash = (value) => createHash("sha256").update(value).digest("hex");

test("external LM requires exact active token and BPE hashes", () => {
  const root = mkdtempSync(path.join(tmpdir(), "newo-lm-")); const asr = path.join(root, "asr"); const lm = path.join(root, "lm");
  mkdirSync(asr); mkdirSync(lm); writeFileSync(path.join(asr, "tokens.txt"), "tokens"); writeFileSync(path.join(asr, "bpe.vocab"), "bpe");
  writeFileSync(path.join(lm, "model.int8.onnx"), "onnx");
  writeFileSync(path.join(lm, "lm.json"), JSON.stringify({ type: "onnx_rnn", model: "model.int8.onnx",
    tokens_sha256: hash("tokens"), bpe_vocab_sha256: hash("bpe") }));
  const valid = resolveSherpaLmConfig({ enabled: true, lmPath: lm, modelDirectory: asr, scale: 0.2, runtimeSupportsOnlineLm: true });
  assert.equal(valid.active, true); assert.equal(valid.scale, 0.2);
  writeFileSync(path.join(asr, "tokens.txt"), "different");
  assert.equal(resolveSherpaLmConfig({ enabled: true, lmPath: lm, modelDirectory: asr }).reason, "vocabulary_incompatible");
});

test("released Node runtime cannot silently claim online LM support", () => {
  const root = mkdtempSync(path.join(tmpdir(), "newo-lm-runtime-")); const asr = path.join(root, "asr"); const lm = path.join(root, "lm");
  mkdirSync(asr); mkdirSync(lm); writeFileSync(path.join(asr, "tokens.txt"), "tokens"); writeFileSync(path.join(asr, "bpe.vocab"), "bpe");
  writeFileSync(path.join(lm, "model.onnx"), "onnx"); writeFileSync(path.join(lm, "lm.json"), JSON.stringify({ type: "onnx_rnn",
    model: "model.onnx", tokens_sha256: hash("tokens"), bpe_vocab_sha256: hash("bpe") }));
  const result = resolveSherpaLmConfig({ enabled: true, lmPath: lm, modelDirectory: asr });
  assert.equal(result.active, false); assert.equal(result.reason, "runtime_unsupported");
});

test("missing or disabled LM fails open and beam width 8 remains configurable", () => {
  assert.equal(resolveSherpaLmConfig({ enabled: false }).reason, "disabled");
  assert.equal(resolveSherpaLmConfig({ enabled: true, lmPath: "missing", modelDirectory: "missing" }).active, false);
  const config = applySherpaDecoderConfig({}, { libriGiga: true, hotwordsFile: "hotwords", fileExists: () => true,
    maxActivePaths: 8, lm: { active: true, model: "/external/lm.onnx", scale: 0.1 } });
  assert.equal(config.decodingMethod, "modified_beam_search"); assert.equal(config.maxActivePaths, 8);
  assert.deepEqual(config.lmConfig, { model: "/external/lm.onnx", scale: 0.1 });
});
