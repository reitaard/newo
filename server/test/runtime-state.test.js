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
    assert.deepEqual(JSON.parse(await readFile(filePath, "utf8")), { speakerEnabled: false, trackDesired: true, telegramUpdateIds: [] });
    const restarted = createRuntimeStateStore({ filePath });
    assert.equal(restarted.speakerEnabled, false);
    assert.equal(restarted.trackDesired, true);
    assert.deepEqual(await Promise.all([restarted.toggleSpeakerEnabled(), restarted.toggleSpeakerEnabled()]), [true, false]);
    assert.equal(JSON.parse(await readFile(filePath, "utf8")).speakerEnabled, false);
    assert.equal(await restarted.acceptTelegramUpdate(123), true);
    assert.equal(await restarted.acceptTelegramUpdate(123), false);
    const afterUpdate = createRuntimeStateStore({ filePath });
    assert.equal(await afterUpdate.acceptTelegramUpdate(123), false);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
