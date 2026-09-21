import { mkdir, rename, writeFile } from "node:fs/promises";
import { readFileSync } from "node:fs";
import path from "node:path";

const ASSISTANT_MODES = new Set(["alfred", "xiaomei"]);
let activeRuntimeStateStore = null;

export function getActiveRuntimeStateStore() { return activeRuntimeStateStore; }

export function createRuntimeStateStore({ filePath, logger, defaults = { speakerEnabled: true, assistantProfile: "qwen3:0.6b", assistantProfileOverrides: {}, assistantMode: "alfred", trackDesired: false, telegramUpdateIds: [] } }) {
  const resolvedPath = path.resolve(filePath);
  let state = { ...defaults };
  try {
    const parsed = JSON.parse(readFileSync(resolvedPath, "utf8"));
    if (typeof parsed?.speakerEnabled === "boolean") state.speakerEnabled = parsed.speakerEnabled;
    if (typeof parsed?.assistantProfile === "string" && parsed.assistantProfile.trim()) state.assistantProfile = parsed.assistantProfile;
    if (parsed?.assistantProfileOverrides && typeof parsed.assistantProfileOverrides === "object" && !Array.isArray(parsed.assistantProfileOverrides)) state.assistantProfileOverrides = parsed.assistantProfileOverrides;
    if (typeof parsed?.assistantMode === "string" && ASSISTANT_MODES.has(parsed.assistantMode.toLowerCase())) state.assistantMode = parsed.assistantMode.toLowerCase();
    if (typeof parsed?.trackDesired === "boolean") state.trackDesired = parsed.trackDesired;
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

  const store = {
    get speakerEnabled() { return state.speakerEnabled; },
    get assistantProfile() { return state.assistantProfile; },
    get assistantProfileOverrides() { return structuredClone(state.assistantProfileOverrides ?? {}); },
    get assistantMode() { return ASSISTANT_MODES.has(state.assistantMode) ? state.assistantMode : "alfred"; },
    get trackDesired() { return state.trackDesired; },
    setSpeakerEnabled: (enabled) => enqueue(async () => (await persist({ speakerEnabled: Boolean(enabled) })).speakerEnabled),
    toggleSpeakerEnabled: () => enqueue(async () => (await persist({ speakerEnabled: !state.speakerEnabled })).speakerEnabled),
    setAssistantProfile: (assistantProfile) => enqueue(async () =>
      (await persist({ assistantProfile: String(assistantProfile) })).assistantProfile),
    setAssistantProfileOverrides: (assistantProfileOverrides) => enqueue(async () =>
      structuredClone((await persist({ assistantProfileOverrides: structuredClone(assistantProfileOverrides ?? {}) })).assistantProfileOverrides)),
    setAssistantMode: (assistantMode) => enqueue(async () => {
      const normalized = String(assistantMode ?? "").trim().toLowerCase();
      if (!ASSISTANT_MODES.has(normalized)) throw new TypeError("invalid assistant mode");
      return (await persist({ assistantMode: normalized })).assistantMode;
    }),
    setTrackDesired: (enabled) => enqueue(async () => (await persist({ trackDesired: Boolean(enabled) })).trackDesired),
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
  activeRuntimeStateStore = store;
  return store;
}
