import assert from "node:assert/strict";
import test from "node:test";

import { formatBytes, formatDuration, formatRecordingCaption } from "../src/newo2-recording-ui.js";

test("Newo2 recording UI formats useful metadata", () => {
  assert.equal(formatBytes(10 * 1024 * 1024), "10.00 MB");
  assert.equal(formatDuration(30_000), "30.0s");

  const caption = formatRecordingCaption({
    fps: 20,
    resolution: "vga",
    quality: 12,
    uploadedBytes: 10 * 1024 * 1024,
  }, {
    duration_ms: 30_000,
    frames: 600,
    dropped: 0,
    reason: "duration_complete",
  });

  assert.match(caption, /VGA · 640×480/);
  assert.match(caption, /20 FPS/);
  assert.match(caption, /JPEG Q12/);
  assert.match(caption, /600/);
  assert.match(caption, /10\.00 MB/);
  assert.match(caption, /SD → VPS → MP4 → Telegram/);
});
