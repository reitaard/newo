const CLOCK_INTENTS = new Set([
  "not_clock",
  "current_time",
  "current_date",
  "create_timer",
  "create_alarm",
  "cancel",
  "dismiss",
  "snooze",
  "pause_timer",
  "resume_timer",
  "start_stopwatch",
  "pause_stopwatch",
  "resume_stopwatch",
  "reset_stopwatch",
  "status",
]);

const OUTPUT_SCHEMA = {
  type: "object",
  additionalProperties: false,
  required: [
    "intent",
    "duration_s",
    "target",
    "hour",
    "minute",
    "meridiem",
    "day",
  ],
  properties: {
    intent: {
      type: "string",
      enum: [...CLOCK_INTENTS],
    },
    duration_s: {
      anyOf: [
        { type: "integer", minimum: 1, maximum: 604800 },
        { type: "null" },
      ],
    },
    target: {
      anyOf: [
        { type: "string", enum: ["alarm", "timer"] },
        { type: "null" },
      ],
    },
    hour: {
      anyOf: [
        { type: "integer", minimum: 1, maximum: 12 },
        { type: "null" },
      ],
    },
    minute: {
      anyOf: [
        { type: "integer", minimum: 0, maximum: 59 },
        { type: "null" },
      ],
    },
    meridiem: {
      anyOf: [
        { type: "string", enum: ["am", "pm"] },
        { type: "null" },
      ],
    },
    day: {
      anyOf: [
        { type: "string", enum: ["today", "tomorrow", "unsupported"] },
        { type: "null" },
      ],
    },
  },
};

export function isClockSemanticCandidate(text) {
  const value = String(text ?? "").toLowerCase().trim();

  // Explicit clock vocabulary is always worth semantic recovery.
  if (/\b(timer|alarm|stopwatch|snooze|wake me|wake up)\b/.test(value))
    return true;

  // Also allow direct action-shaped duration requests such as
  // "please start five minutes for me". MiniCPM still decides whether this
  // is actually a clock action; this only opens the semantic fallback lane.
  return /^(?:please\s+)?(?:(?:can|could|would)\s+you\s+)?(?:start|set|give me)\b/.test(value) &&
    /\b(?:seconds?|minutes?|hours?)\b/.test(value);
}

function validateResult(value) {
  if (!value || typeof value !== "object" || Array.isArray(value))
    return { intent: "not_clock" };

  if (!CLOCK_INTENTS.has(value.intent))
    return { intent: "not_clock" };

  const result = {
    intent: value.intent,
    duration_s: Number.isInteger(value.duration_s) ? value.duration_s : null,
    target: ["alarm", "timer"].includes(value.target) ? value.target : null,
    hour: Number.isInteger(value.hour) ? value.hour : null,
    minute: Number.isInteger(value.minute) ? value.minute : null,
    meridiem: ["am", "pm"].includes(value.meridiem) ? value.meridiem : null,
    day: ["today", "tomorrow", "unsupported"].includes(value.day)
      ? value.day
      : null,
  };

  if (
    result.duration_s != null &&
    (result.duration_s < 1 || result.duration_s > 7 * 24 * 3600)
  ) result.duration_s = null;

  if (
    result.hour != null &&
    (result.hour < 1 || result.hour > 12)
  ) result.hour = null;

  if (
    result.minute != null &&
    (result.minute < 0 || result.minute > 59)
  ) result.minute = null;

  // Canonicalize slots by intent. The LLM identifies meaning; the host owns
  // validation and execution.
  if (result.intent !== "create_timer" && result.intent !== "snooze")
    result.duration_s = null;

  if (result.intent !== "create_alarm") {
    result.hour = null;
    result.minute = null;
    result.meridiem = null;
    result.day = null;
  }

  if (result.intent !== "cancel")
    result.target = null;

  return result;
}

export function createClockSemanticResolver({
  profile,
  fetchImpl = fetch,
  timeoutMs = 3000,
  logger = null,
} = {}) {
  if (!profile?.baseUrl || !profile?.model)
    return async () => ({ intent: "not_clock" });

  const endpoint =
    `${String(profile.baseUrl).replace(/\/+$/, "")}/api/chat`;

  return async function resolveClockSemantic(text) {
    if (!isClockSemanticCandidate(text))
      return { intent: "not_clock" };

    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    timer.unref?.();

    const started = performance.now();

    try {
      const response = await fetchImpl(endpoint, {
        method: "POST",
        headers: { "content-type": "application/json" },
        signal: controller.signal,
        body: JSON.stringify({
          model: profile.model,
          stream: false,
          think: false,
          keep_alive: profile.keepAlive ?? -1,
          format: OUTPUT_SCHEMA,
          options: {
            temperature: 0,
            num_predict: 96,
          },
          messages: [
            {
              role: "system",
              content:
                "You are a strict clock-intent parser for a voice assistant. " +
                "Interpret likely speech-recognition mistakes by intended meaning, but never execute anything and never claim success. " +
                "Return only the requested JSON schema. " +

                "IMPORTANT: distinguish ACTION REQUESTS from DISCUSSION. " +
                "If the user is asking the assistant to actually create, change, cancel, pause, resume, dismiss, snooze, start, or query a clock feature, return that action intent. " +
                "An action request with missing information is STILL the action intent; put the missing slot as null. " +
                "Do NOT return not_clock merely because AM/PM, a duration, or another required slot is missing. The host will ask for clarification. " +
                "Return not_clock only for instructional, hypothetical, explanatory, programming, comparison, or general discussion about clocks, timers, alarms, dates, or stopwatches. " +

                "Convert spoken number words into numeric slots. Examples: seven means hour 7; ten seconds means duration_s 10; five minutes means duration_s 300. " +

                "Examples: " +
                "'give me a ten second timer' => create_timer with duration_s 10. " +
                "'SAID THE TIMER FOR TEN SECONDS' when it clearly means an ASR-corrupted request to set a timer => create_timer with duration_s 10. " +
                "'set an alarm for seven tomorrow' => create_alarm, hour 7, minute 0, meridiem null, day tomorrow. " +
                "'wake me at seven AM tomorrow' => create_alarm, hour 7, minute 0, meridiem am, day tomorrow. " +
                "'how do I set a timer in JavaScript' => not_clock. " +
                "'what are smoke alarms' => not_clock. " +

                "For create_timer, duration_s is the requested total duration. " +
                "For create_alarm, use hour, minute, meridiem, and day only; duration_s must be null. " +
                "Never calculate Unix timestamps or UTC times. " +
                "Never infer AM or PM when the user did not state it. " +
                "day may only be today, tomorrow, unsupported, or null. " +
                "Weekdays, explicit calendar dates, next week, and phrases such as day after tomorrow use day=unsupported. " +
                "Unused fields must be null.",
            },
            {
              role: "user",
              content: String(text ?? "").slice(0, 500),
            },
          ],
        }),
      });

      if (!response.ok)
        throw new Error(`clock_semantic_http_${response.status}`);

      const payload = await response.json();
      const content = payload?.message?.content;

      if (typeof content !== "string")
        throw new Error("clock_semantic_invalid_response");

      const result = validateResult(JSON.parse(content));

      logger?.info?.({
        query_text: String(text ?? "").slice(0, 160),
        semantic_intent: result.intent,
        semantic_ms: Math.round(performance.now() - started),
        model: profile.model,
      }, "Clock semantic fallback");

      return result;
    } catch (error) {
      logger?.warn?.({
        error: error?.message ?? String(error),
        semantic_ms: Math.round(performance.now() - started),
      }, "Clock semantic fallback failed");

      return { intent: "not_clock" };
    } finally {
      clearTimeout(timer);
    }
  };
}
