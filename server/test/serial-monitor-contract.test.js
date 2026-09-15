import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const read = (path) => readFile(new URL(path, import.meta.url), "utf8");

test("serial monitor is demand-driven and relays bounded binary frames", async () => {
  const [server, cloud, consoleSource, consoleHeader] = await Promise.all([
    read("../src/index.js"), read("../../Newo/newo_cloud.cpp"),
    read("../../Newo/newo_console.cpp"), read("../../Newo/newo_console.h"),
  ]);
  assert.match(server, /serialMonitorWss\.clients\.size > 0/);
  assert.match(server, /serial_monitor_control/);
  assert.match(server, /client\.bufferedAmount < 64 \* 1024/);
  assert.match(server, /frame\.length >= 12/);
  assert.match(cloud, /static uint8_t frame\[12 \+ 1024\]/);
  assert.match(cloud, /sendBIN\(frame, bytes \+ 12\)/);
  assert.match(consoleHeader, /remoteCapacity\(\).*32 \* 1024/);
  assert.match(consoleSource, /MALLOC_CAP_SPIRAM/);
  assert.doesNotMatch(consoleSource, /WebSocketsClient/);
});

test("serial monitor page uses same-origin websocket and bounded display history", async () => {
  const [html, browser] = await Promise.all([
    read("../public/smonitor.html"), read("../public/smonitor.js"),
  ]);
  assert.match(html, /Newo Serial Monitor/);
  assert.match(browser, /location\.host.*smonitor\/ws/);
  assert.match(browser, /1_000_000/);
  assert.match(browser, /console bytes dropped/);
  assert.match(html, /role="toolbar"/);
  assert.match(html, /id="autoscroll".*aria-pressed="true"/);
  assert.match(html, /id="copy"/);
  assert.match(html, /id="clear"/);
  assert.match(browser, /navigator\.clipboard\.writeText/);
  assert.match(browser, /document\.execCommand\("copy"\)/);
  assert.match(browser, /function lineClass/);
  assert.match(browser, /rawLog\.slice\(-750_000\)/);
});
