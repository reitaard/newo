import { createHash } from "node:crypto";
import { existsSync, readFileSync, statSync } from "node:fs";
import path from "node:path";

function sha256(file) { return createHash("sha256").update(readFileSync(file)).digest("hex"); }

export function resolveSherpaLmConfig({ enabled = false, lmPath = "", type = "onnx_rnn", scale = 0.1,
  modelDirectory, runtimeSupportsOnlineLm = false, logger = null } = {}) {
  const inactive = (reason) => ({ requested: Boolean(enabled), active: false, type, path: lmPath || null, scale, reason });
  if (!enabled) return inactive("disabled");
  try {
    if (!lmPath) return inactive("path_missing");
    const directory = path.resolve(lmPath);
    if (!existsSync(directory) || !statSync(directory).isDirectory()) return inactive("model_missing");
    const manifestPath = path.join(directory, "lm.json");
    if (!existsSync(manifestPath)) return inactive("manifest_missing");
    const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
    if (manifest.type !== type || manifest.interface !== "online_rnn_stateful_v1" || typeof manifest.model !== "string")
      return inactive("manifest_incompatible");
    const model = path.resolve(directory, manifest.model);
    if (!model.startsWith(`${directory}${path.sep}`) || !existsSync(model)) return inactive("model_missing");
    const tokens = path.resolve(modelDirectory, "tokens.txt");
    const bpe = path.resolve(modelDirectory, "bpe.vocab");
    if (!existsSync(tokens) || !existsSync(bpe)) return inactive("asr_vocabulary_missing");
    if (manifest.tokens_sha256 !== sha256(tokens) || manifest.bpe_vocab_sha256 !== sha256(bpe))
      return inactive("vocabulary_incompatible");
    if (!runtimeSupportsOnlineLm) return inactive("runtime_unsupported");
    return { requested: true, active: true, type, path: directory, model, scale, reason: null };
  } catch (error) {
    logger?.warn?.({ event: "SHERPA_LM_UNAVAILABLE", reason: "validation_failed", error_message: error.message }, "SHERPA_LM_UNAVAILABLE");
    return inactive("validation_failed");
  }
}
