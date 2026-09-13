import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const root = new URL("../../", import.meta.url);
const source = (path) => readFileSync(new URL(path, root), "utf8");

test("USB boot controls persist and gate each real client", () => {
  const ino = source("Newo/Newo.ino");
  const storage = source("Newo/newo_storage.cpp");
  assert.match(ino, /if \(!newoStorage\.usbHostEnabled\(\)\)/);
  assert.match(ino, /newoStorage\.usbAudioEnabled\(\) && !newoUsbAudio\.begin/);
  assert.match(ino, /newoStorage\.usbStorageEnabled\(\) && !newoUsbStorage\.begin/);
  assert.match(ino, /if \(newoStorage\.usbVcpEnabled\(\)\)/);
  assert.match(storage, /usb-host-on/);
  assert.match(storage, /usb-audio-on/);
  assert.match(storage, /usb-store-on/);
  assert.match(storage, /usb-vcp-on/);
});

test("USB enable trial automatically rolls back without cloud authentication", () => {
  const ino = source("Newo/Newo.ino");
  assert.match(ino, /usbTrialPending\(\)/);
  assert.match(ino, /newoCloud\.ready\(\)/);
  assert.match(ino, /TRIAL_CONFIRMED/);
  assert.match(ino, /TRIAL_ROLLBACK/);
  assert.match(ino, /setUsbHostEnabled\(false\)/);
});
