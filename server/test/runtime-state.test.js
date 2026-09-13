import assert from "node:assert/strict";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { createRuntimeStateStore } from "../src/runtime-state.js";

test("automatic speaker state persists across server restart", async () => {
  const directory = await mkdtemp(path.join(os.tmpdir(), "newo-state-"));
  try {
    const filePath = path.join(directory, "runtime-state.json");
    const first = createRuntimeStateStore({ filePath });
    assert.equal(first.speakerEnabled, true);
    assert.equal(first.trackDesired, false);
    await first.setTrackDesired(true);
    await first.setSpeakerEnabled(false);
    assert.equal(JSON.parse(await readFile(filePath, "utf8")).speakerEnabled, false);
    const restarted = createRuntimeStateStore({ filePath });
    assert.equal(restarted.speakerEnabled, false);
    assert.equal(restarted.trackDesired, true);
    assert.deepEqual(await Promise.all([restarted.toggleSpeakerEnabled(), restarted.toggleSpeakerEnabled()]), [true, false]);
    assert.equal(JSON.parse(await readFile(filePath, "utf8")).speakerEnabled, false);
    assert.equal(await restarted.acceptTelegramUpdate(123), true);
    assert.equal(await restarted.acceptTelegramUpdate(123), false);
    assert.equal(await createRuntimeStateStore({ filePath }).acceptTelegramUpdate(123), false);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

test("preferred assistant profile persists without changing speaker state", async () => {
  const directory = await mkdtemp(path.join(os.tmpdir(), "newo-profile-state-"));
  try {
    const filePath = path.join(directory, "runtime-state.json");
    const first = createRuntimeStateStore({ filePath });
    await first.setAssistantProfile("lfm2.5:8b");
    await first.setAssistantProfileOverrides({ "lfm2.5:8b": { max_tokens: 48 } });
    assert.equal(first.assistantProfile, "lfm2.5:8b");
    assert.equal(first.speakerEnabled, true);
    const restarted = createRuntimeStateStore({ filePath });
    assert.equal(restarted.assistantProfile, "lfm2.5:8b");
    assert.deepEqual(restarted.assistantProfileOverrides, { "lfm2.5:8b": { max_tokens: 48 } });
    assert.equal(restarted.speakerEnabled, true);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
