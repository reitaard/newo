import { mkdir, rename, writeFile } from "node:fs/promises";
import { readFileSync } from "node:fs";
import path from "node:path";

export function createRuntimeStateStore({ filePath, logger, defaults = { speakerEnabled: true, assistantProfile: "qwen3:0.6b" } }) {
  const resolvedPath = path.resolve(filePath);
  let state = { ...defaults };
  try {
    const parsed = JSON.parse(readFileSync(resolvedPath, "utf8"));
    if (typeof parsed?.speakerEnabled === "boolean") state.speakerEnabled = parsed.speakerEnabled;
    if (typeof parsed?.assistantProfile === "string" && parsed.assistantProfile.trim()) state.assistantProfile = parsed.assistantProfile;
  } catch (error) {
    if (error?.code !== "ENOENT") logger?.warn?.({ state_file: resolvedPath, error_message: error?.message ?? "unknown" }, "Ignoring invalid runtime state");
  }

  let persistenceQueue = Promise.resolve();

  async function persist(patch) {
    const next = { ...state, ...patch };
    const temporaryPath = `${resolvedPath}.${process.pid}.tmp`;
    await mkdir(path.dirname(resolvedPath), { recursive: true });
    await writeFile(temporaryPath, `${JSON.stringify(next, null, 2)}\n`, { mode: 0o600 });
    await rename(temporaryPath, resolvedPath);
    state = next;
    return state;
  }

  function enqueue(update) {
    const operation = persistenceQueue.then(update);
    persistenceQueue = operation.catch(() => {});
    return operation;
  }

  return {
    get speakerEnabled() { return state.speakerEnabled; },
    get assistantProfile() { return state.assistantProfile; },
    setSpeakerEnabled: (enabled) => enqueue(async () => (await persist({ speakerEnabled: Boolean(enabled) })).speakerEnabled),
    toggleSpeakerEnabled: () => enqueue(async () => (await persist({ speakerEnabled: !state.speakerEnabled })).speakerEnabled),
    setAssistantProfile: (assistantProfile) => enqueue(async () =>
      (await persist({ assistantProfile: String(assistantProfile) })).assistantProfile),
    path: resolvedPath,
  };
}
