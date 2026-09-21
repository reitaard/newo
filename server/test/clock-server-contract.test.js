import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

test("server and clock handler agree on clock_command_ack protocol", async () => {
  const [indexSource, clockSource] = await Promise.all([
    readFile(new URL("../src/index.js", import.meta.url), "utf8"),
    readFile(new URL("../src/clock-transcript.js", import.meta.url), "utf8"),
  ]);

  // Existing display-clock protocol remains intact.
  assert.match(
    indexSource,
    /z\.literal\("clock_ack"\)/
  );

  // Clock engine command ACK emitted by ESP is accepted by server.
  assert.match(
    indexSource,
    /z\.literal\("clock_command_ack"\)/
  );

  // Clock handler waits for that exact ACK type.
  assert.match(
    clockSource,
    /sendDeviceRequest\(\s*"clock_command",\s*"clock_command_ack"/
  );
});
