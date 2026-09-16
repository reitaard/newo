import { mkdir, rename, writeFile } from "node:fs/promises";
import { readFileSync } from "node:fs";
import path from "node:path";

export function createRuntimeStateStore({ filePath, logger, defaults = { speakerEnabled: true, assistantProfile: "qwen3:0.6b", assistantProfileOverrides: {}, trackDesired: false, cameraFlip: true, cameraMirror: false, telegramUpdateIds: [] } }) {
  const resolvedPath = path.resolve(filePath);
  let state = { ...defaults };
  try {
    const parsed = JSON.parse(readFileSync(resolvedPath, "utf8"));
    if (typeof parsed?.speakerEnabled === "boolean") state.speakerEnabled = parsed.speakerEnabled;
    if (typeof parsed?.assistantProfile === "string" && parsed.assistantProfile.trim()) state.assistantProfile = parsed.assistantProfile;
    if (parsed?.assistantProfileOverrides && typeof parsed.assistantProfileOverrides === "object" && !Array.isArray(parsed.assistantProfileOverrides)) state.assistantProfileOverrides = parsed.assistantProfileOverrides;
    if (typeof parsed?.trackDesired === "boolean") state.trackDesired = parsed.trackDesired;
    if (typeof parsed?.cameraFlip === "boolean") state.cameraFlip = parsed.cameraFlip;
    if (typeof parsed?.cameraMirror === "boolean") state.cameraMirror = parsed.cameraMirror;
    if (Array.isArray(parsed?.telegramUpdateIds)) {
      state.telegramUpdateIds = parsed.telegramUpdateIds.filter((value) => Number.isSafeInteger(value) && value >= 0).slice(-512);
    }
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
    get assistantProfileOverrides() { return structuredClone(state.assistantProfileOverrides ?? {}); },
    get trackDesired() { return state.trackDesired; },
    get cameraFlip() { return state.cameraFlip; },
    get cameraMirror() { return state.cameraMirror; },
    setSpeakerEnabled: (enabled) => enqueue(async () => (await persist({ speakerEnabled: Boolean(enabled) })).speakerEnabled),
    toggleSpeakerEnabled: () => enqueue(async () => (await persist({ speakerEnabled: !state.speakerEnabled })).speakerEnabled),
    setAssistantProfile: (assistantProfile) => enqueue(async () =>
      (await persist({ assistantProfile: String(assistantProfile) })).assistantProfile),
    setAssistantProfileOverrides: (assistantProfileOverrides) => enqueue(async () =>
      structuredClone((await persist({ assistantProfileOverrides: structuredClone(assistantProfileOverrides ?? {}) })).assistantProfileOverrides)),
    setTrackDesired: (enabled) => enqueue(async () => (await persist({ trackDesired: Boolean(enabled) })).trackDesired),
    setCameraFlip: (enabled) => enqueue(async () => (await persist({ cameraFlip: Boolean(enabled) })).cameraFlip),
    setCameraMirror: (enabled) => enqueue(async () => (await persist({ cameraMirror: Boolean(enabled) })).cameraMirror),
    acceptTelegramUpdate: (updateId, limit = 512) => enqueue(async () => {
      if (!Number.isSafeInteger(updateId) || updateId < 0) throw new TypeError("invalid Telegram update_id");
      const retained = Array.isArray(state.telegramUpdateIds) ? state.telegramUpdateIds : [];
      if (retained.includes(updateId)) return false;
      const boundedLimit = Math.max(16, Math.min(4096, Number(limit) || 512));
      await persist({ telegramUpdateIds: [...retained, updateId].slice(-boundedLimit) });
      return true;
    }),
    path: resolvedPath,
  };
}
