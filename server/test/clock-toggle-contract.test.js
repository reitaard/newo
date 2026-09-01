import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const source = async (path) => readFile(new URL(path, import.meta.url), "utf8");

test("clock control is correlated, firmware-persisted, and does not gate time synchronization", async () => {
  const [index, commands, cloud, storage, clock] = await Promise.all([
    source("../src/index.js"),
    source("../src/telegram-mode-commands.js"),
    source("../../Newo/newo_cloud.cpp"),
    source("../../Newo/newo_storage.cpp"),
    source("../../Newo/newo_clock.cpp"),
  ]);
  assert.match(index, /z\.object\(\{ type: z\.literal\("clock_ack"\), request_id: z\.string\(\), enabled: z\.boolean\(\), applied: z\.boolean\(\) \}\)/);
  assert.match(index, /bot\.command\("clock", primaryModeHandlers\.clock\)/);
  assert.match(commands, /sendDeviceRequest\("clock_control", "clock_ack", \{ action: parsed\.kind \}/);
  assert.match(cloud, /if \(strcmp\(type, "clock_control"\) == 0\)/);
  assert.match(cloud, /sendClockAck\(requestId, display_\.clockEnabled\(\), applied\)/);
  assert.match(storage, /constexpr char kClockEnabledKey\[\] = "clock-on"/);
  assert.match(storage, /preferences_\.getBool\(kClockEnabledKey, true\)/);
  assert.match(storage, /bool NewoStorage::setClockEnabled\(bool enabled\)/);
  assert.match(clock, /configTzTime\(kTimeZone, "pool\.ntp\.org", "time\.nist\.gov"\)/);
  assert.match(clock, /if \(!clockEnabled_\)/);
});
