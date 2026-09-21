import test from "node:test";
import assert from "node:assert/strict";
import { createClockTranscriptHandler } from "../src/clock-transcript.js";

function harness() {
  const calls = { assistant: [], device: [], spoken: [], states: [] };
  const handler = createClockTranscriptHandler({
    timeZone: "Asia/Bangkok",
    now: () => new Date("2026-09-21T03:00:00Z"),
    sendDeviceRequest(type, responseType, fields) {
      calls.device.push({ type, responseType, fields });
      return { kind: "sent", promise: Promise.resolve({ kind: "response", message: { applied: true } }) };
    },
    speakerRuntime: { speak(text) { calls.spoken.push(text); return { kind: "queued", completion: Promise.resolve() }; } },
    isPersistentSpeakerEnabled: () => true,
    sendAssistantState(deviceId, state) { calls.states.push({ deviceId, state }); },
  });
  const assistant = { async respond(turn) { calls.assistant.push(turn.text); } };
  const route = async (text) => {
    const turn = { text, deviceId: "newo", streamId: "stream" };
    if (!await handler(turn)) await assistant.respond(turn);
  };
  return { calls, route };
}

test("recognized clock transcripts bypass the assistant and send typed device commands", async () => {
  const { calls, route } = harness();
  await route("set a timer for ten minutes");
  assert.deepEqual(calls.assistant, []);
  assert.deepEqual(calls.device, [{ type: "clock_command", responseType: "clock_command_ack",
    fields: { action: "create_timer", duration_s: 600 } }]);
  assert.deepEqual(calls.spoken, ["Timer set for 10 minutes."]);
});

test("non-clock transcripts continue to the normal assistant", async () => {
  const { calls, route } = harness();
  await route("explain time dilation");
  assert.deepEqual(calls.assistant, ["explain time dilation"]);
  assert.deepEqual(calls.device, []);
  assert.deepEqual(calls.spoken, []);
});

test("instructional timer questions reach the assistant without a clock device command", async () => {
  const { calls, route } = harness();
  await route("how do I set a timer in JavaScript");
  assert.deepEqual(calls.assistant, ["how do I set a timer in JavaScript"]);
  assert.deepEqual(calls.device, []);
  assert.deepEqual(calls.spoken, []);
});

test("ambiguous clock transcripts clarify without assistant or device execution", async () => {
  const { calls, route } = harness();
  await route("set an alarm for seven");
  assert.deepEqual(calls.assistant, []);
  assert.deepEqual(calls.device, []);
  assert.deepEqual(calls.spoken, ["Please say AM or PM."]);
});

test("noisy ASR clock request can recover semantically without normal assistant execution", async () => {
  const calls = {
    device: [],
    spoken: [],
    semantic: [],
  };

  const handler = createClockTranscriptHandler({
    timeZone: "Asia/Phnom_Penh",
    now: () => new Date("2026-09-21T18:30:00Z"),

    semanticResolve: async (text) => {
      calls.semantic.push(text);

      return {
        intent: "create_timer",
        duration_s: 10,
        target: null,
        hour: null,
        minute: null,
        meridiem: null,
        day: null,
      };
    },

    sendDeviceRequest(type, responseType, fields) {
      calls.device.push({
        type,
        responseType,
        fields,
      });

      return {
        kind: "sent",
        promise: Promise.resolve({
          kind: "response",
          message: {
            type: "clock_command_ack",
            applied: true,
          },
        }),
      };
    },

    speakerRuntime: {
      speak(text) {
        calls.spoken.push(text);

        return {
          kind: "queued",
          completion: Promise.resolve(),
        };
      },
    },

    isPersistentSpeakerEnabled: () => true,
    sendAssistantState() {},
  });

  const handled = await handler({
    text: "SAID THE TIMER FOR TEN SECONDS",
    deviceId: "newo-01",
    streamId: "semantic-test",
  });

  assert.equal(handled, true);

  assert.deepEqual(
    calls.semantic,
    ["SAID THE TIMER FOR TEN SECONDS"]
  );

  assert.deepEqual(
    calls.device,
    [{
      type: "clock_command",
      responseType: "clock_command_ack",
      fields: {
        action: "create_timer",
        duration_s: 10,
      },
    }]
  );

  assert.deepEqual(
    calls.spoken,
    ["Timer set for 10 seconds."]
  );
});

test("clock command rejection never speaks a success confirmation", async () => {
  const spoken = [];

  const handler = createClockTranscriptHandler({
    timeZone: "Asia/Phnom_Penh",

    sendDeviceRequest() {
      return {
        kind: "sent",
        promise: Promise.resolve({
          kind: "response",
          message: {
            type: "clock_command_ack",
            applied: false,
            error: "timer_limit",
          },
        }),
      };
    },

    speakerRuntime: {
      speak(text) {
        spoken.push(text);

        return {
          kind: "queued",
          completion: Promise.resolve(),
        };
      },
    },

    isPersistentSpeakerEnabled: () => true,
    sendAssistantState() {},
  });

  await handler({
    text: "set a timer for ten seconds",
    deviceId: "newo-01",
    streamId: "reject-test",
  });

  assert.deepEqual(
    spoken,
    ["The clock request was not accepted."]
  );
});
