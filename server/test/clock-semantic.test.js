import test from "node:test";
import assert from "node:assert/strict";

import {
  createClockSemanticResolver,
  isClockSemanticCandidate,
} from "../src/clock-semantic.js";

import {
  clockRequestFromSemantic,
  formatClockReply,
} from "../src/clock-request.js";

const boundaryNow = new Date("2026-09-21T18:30:00Z");
const localOptions = {
  now: boundaryNow,
  timeZone: "Asia/Phnom_Penh",
};

test("semantic candidate detection is broad enough for clock actions but avoids discussion", () => {
  assert.equal(
    isClockSemanticCandidate("SAID THE TIMER FOR TEN SECONDS"),
    true
  );

  assert.equal(
    isClockSemanticCandidate("give me a ten second timer"),
    true
  );

  assert.equal(
    isClockSemanticCandidate("wake me tomorrow morning"),
    true
  );

  assert.equal(
    isClockSemanticCandidate("explain time dilation"),
    false
  );
});

test("semantic timer slots become a validated deterministic command", () => {
  assert.deepEqual(
    clockRequestFromSemantic(
      {
        intent: "create_timer",
        duration_s: 10,
        target: null,
        hour: null,
        minute: null,
        meridiem: null,
        day: null,
      },
      localOptions
    ),
    {
      kind: "command",
      action: "create_timer",
      duration_s: 10,
    }
  );
});

test("semantic alarm slots use Phnom Penh wall time, not UTC wall time", () => {
  const request = clockRequestFromSemantic(
    {
      intent: "create_alarm",
      duration_s: null,
      target: null,
      hour: 7,
      minute: 0,
      meridiem: "am",
      day: "tomorrow",
    },
    localOptions
  );

  assert.equal(request.kind, "command");
  assert.equal(request.action, "create_alarm");

  // At boundaryNow Phnom Penh is already September 22.
  // Tomorrow 07:00 ICT = September 23 00:00 UTC.
  assert.equal(
    new Date(request.epoch_s * 1000).toISOString(),
    "2026-09-23T00:00:00.000Z"
  );
});

test("semantic alarm never guesses missing AM or PM", () => {
  assert.deepEqual(
    clockRequestFromSemantic(
      {
        intent: "create_alarm",
        duration_s: null,
        target: null,
        hour: 7,
        minute: 0,
        meridiem: null,
        day: "tomorrow",
      },
      localOptions
    ),
    {
      kind: "ambiguous",
      message: "Please say AM or PM.",
    }
  );
});

test("unsupported semantic alarm dates fail closed", () => {
  assert.deepEqual(
    clockRequestFromSemantic(
      {
        intent: "create_alarm",
        duration_s: null,
        target: null,
        hour: 7,
        minute: 0,
        meridiem: "am",
        day: "unsupported",
      },
      localOptions
    ),
    {
      kind: "ambiguous",
      message: "Please give a supported date such as today or tomorrow.",
    }
  );
});

test("semantic current date remains deterministic and complete", () => {
  const request = clockRequestFromSemantic(
    {
      intent: "current_date",
      duration_s: null,
      target: null,
      hour: null,
      minute: null,
      meridiem: null,
      day: null,
    },
    localOptions
  );

  assert.equal(
    formatClockReply(
      request,
      { applied: true },
      localOptions
    ),
    "Today is Tuesday, September 22, 2026."
  );
});

test("semantic resolver accepts MiniCPM structured JSON", async () => {
  const requests = [];

  const resolver = createClockSemanticResolver({
    profile: {
      baseUrl: "http://minicpm.test:11435",
      model: "newo-minicpm5:latest",
      keepAlive: -1,
    },

    fetchImpl: async (url, options) => {
      requests.push({
        url,
        body: JSON.parse(options.body),
      });

      return {
        ok: true,
        async json() {
          return {
            message: {
              content: JSON.stringify({
                intent: "create_timer",
                duration_s: 10,
                target: null,
                hour: null,
                minute: null,
                meridiem: null,
                day: null,
              }),
            },
          };
        },
      };
    },
  });

  const result = await resolver(
    "SAID THE TIMER FOR TEN SECONDS"
  );

  assert.equal(requests.length, 1);
  assert.equal(
    requests[0].url,
    "http://minicpm.test:11435/api/chat"
  );

  assert.equal(requests[0].body.think, false);
  assert.equal(requests[0].body.stream, false);
  assert.equal(
    requests[0].body.model,
    "newo-minicpm5:latest"
  );

  assert.equal(
    requests[0].body.format.type,
    "object"
  );

  assert.deepEqual(result, {
    intent: "create_timer",
    duration_s: 10,
    target: null,
    hour: null,
    minute: null,
    meridiem: null,
    day: null,
  });
});

test("instructional timer request may be classified as not_clock", async () => {
  const resolver = createClockSemanticResolver({
    profile: {
      baseUrl: "http://minicpm.test:11435",
      model: "newo-minicpm5:latest",
    },

    fetchImpl: async () => ({
      ok: true,
      async json() {
        return {
          message: {
            content: JSON.stringify({
              intent: "not_clock",
              duration_s: null,
              target: null,
              hour: null,
              minute: null,
              meridiem: null,
              day: null,
            }),
          },
        };
      },
    }),
  });

  const result = await resolver(
    "how do I set a timer in JavaScript"
  );

  assert.equal(result.intent, "not_clock");
});

test("action-shaped duration requests reach semantic recovery without clock keywords", () => {
  assert.equal(
    isClockSemanticCandidate("please start five minutes for me"),
    true
  );

  assert.equal(
    isClockSemanticCandidate("explain what five minutes means"),
    false
  );
});
