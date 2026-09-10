import { mkdir, rename, writeFile } from "node:fs/promises";
import { readFileSync } from "node:fs";
import path from "node:path";

export function createRuntimeStateStore({ filePath, logger, defaults = { speakerEnabled: true, trackDesired: false } }) {
  const resolvedPath = path.resolve(filePath);
  let state = { ...defaults };
  try {
    const parsed = JSON.parse(readFileSync(resolvedPath, "utf8"));
    if (typeof parsed?.speakerEnabled === "boolean") state.speakerEnabled = parsed.speakerEnabled;
    if (typeof parsed?.trackDesired === "boolean") state.trackDesired = parsed.trackDesired;
  } catch (error) {
    if (error?.code !== "ENOENT") logger?.warn?.({ state_file: resolvedPath, error_message: error?.message ?? "unknown" }, "Ignoring invalid runtime state");
  }

  let persistenceQueue = Promise.resolve();

  async function persist(next) {
    const temporaryPath = `${resolvedPath}.${process.pid}.tmp`;
    await mkdir(path.dirname(resolvedPath), { recursive: true });
    await writeFile(temporaryPath, `${JSON.stringify(next, null, 2)}\n`, { mode: 0o600 });
    await rename(temporaryPath, resolvedPath);
    state = next;
  }

  function enqueue(update) {
    const operation = persistenceQueue.then(update);
    persistenceQueue = operation.catch(() => {});
    return operation;
  }

  return {
    get speakerEnabled() { return state.speakerEnabled; },
    get trackDesired() { return state.trackDesired; },
    setSpeakerEnabled: (enabled) => enqueue(async () => {
      await persist({ ...state, speakerEnabled: Boolean(enabled) });
      return state.speakerEnabled;
    }),
    toggleSpeakerEnabled: () => enqueue(async () => {
      await persist({ ...state, speakerEnabled: !state.speakerEnabled });
      return state.speakerEnabled;
    }),
    setTrackDesired: (enabled) => enqueue(async () => {
      await persist({ ...state, trackDesired: Boolean(enabled) });
      return state.trackDesired;
    }),
    path: resolvedPath,
  };
}
