const ONES = Object.freeze({ one: 1, two: 2, three: 3, four: 4, five: 5, six: 6, seven: 7, eight: 8,
  nine: 9, ten: 10, eleven: 11, twelve: 12, thirteen: 13, fourteen: 14, fifteen: 15, sixteen: 16,
  seventeen: 17, eighteen: 18, nineteen: 19 });
const TENS = Object.freeze({ twenty: 20, thirty: 30, forty: 40, fifty: 50 });
const ONES_WORDS = Object.keys(ONES).join("|");
const SMALL_ONES_WORDS = Object.keys(ONES).slice(0, 9).join("|");
const NUMBER_WORD_SOURCE = `(?:(?:twenty|thirty|forty|fifty)(?:[- ](?:${SMALL_ONES_WORDS}))?|${ONES_WORDS}|an?)`;
const HOUR_WORD_SOURCE = Object.keys(ONES).slice(0, 12).join("|");
const UNSUPPORTED_DATE = /\b(?:the\s+day\s+after\s+tomorrow|next\s+week|next\s+(?:monday|tuesday|wednesday|thursday|friday|saturday|sunday)|monday|tuesday|wednesday|thursday|friday|saturday|sunday|january|february|march|april|may|june|july|august|september|october|november|december)\b/i;
const DATE_CLARIFICATION = "Please give a supported date such as today or tomorrow.";

function spokenNumber(value) {
  const normalized = String(value ?? "").toLowerCase().replaceAll("-", " ").trim().replace(/\s+/g, " ");
  if (/^\d+$/.test(normalized)) return Number(normalized);
  if (normalized === "a" || normalized === "an") return 1;
  if (ONES[normalized] !== undefined) return ONES[normalized];
  const [tens, ones] = normalized.split(" ");
  if (TENS[tens] !== undefined && (ones === undefined || ONES[ones] >= 1 && ONES[ones] <= 9))
    return TENS[tens] + (ones ? ONES[ones] : 0);
  return null;
}

function durationSeconds(text) {
  const normalized = String(text).toLowerCase().replace(/\bhalf\s+(?:an?\s+)?hour\b/g, "thirty minutes");
  const matcher = new RegExp(`\\b(${NUMBER_WORD_SOURCE}|\\d{1,4})\\s*(hours?|minutes?|seconds?)\\b`, "gi");
  const units = { hour: 3600, hours: 3600, minute: 60, minutes: 60, second: 1, seconds: 1 };
  let total = 0;
  let matched = false;
  for (const match of normalized.matchAll(matcher)) {
    const amount = spokenNumber(match[1]);
    if (amount === null) continue;
    total += amount * units[match[2].toLowerCase()];
    matched = true;
  }
  return matched && total > 0 && total <= 7 * 24 * 3600 ? total : null;
}

function zonedParts(date, timeZone) {
  const parts = new Intl.DateTimeFormat("en-US", { timeZone, year: "numeric", month: "numeric", day: "numeric",
    hour: "numeric", minute: "numeric", second: "numeric", hourCycle: "h23" }).formatToParts(date);
  return Object.fromEntries(parts.filter(({ type }) => type !== "literal").map(({ type, value }) => [type, Number(value)]));
}

function zonedEpoch({ year, month, day, hour, minute }, timeZone) {
  let guess = Date.UTC(year, month - 1, day, hour, minute, 0);
  for (let index = 0; index < 3; index += 1) {
    const actual = zonedParts(new Date(guess), timeZone);
    const delta = Date.UTC(year, month - 1, day, hour, minute, 0) -
      Date.UTC(actual.year, actual.month - 1, actual.day, actual.hour, actual.minute, actual.second);
    guess += delta;
  }
  return Math.floor(guess / 1000);
}

function parseAlarmTimeTail(tail) {
  const normalized = tail.trim().replace(/[?!.,]+$/g, "").replace(/\s+/g, " ");
  let match = normalized.match(/^(\d{1,2})(?::(\d{1,2}))?\s*(a\.?m\.?|p\.?m\.?)?(?:\s+(.*))?$/i);
  let hour;
  let minute;
  let meridiem;
  let remainder;
  if (match) {
    hour = Number(match[1]);
    minute = Number(match[2] ?? 0);
    meridiem = match[3]?.toLowerCase().replaceAll(".", "") ?? null;
    remainder = match[4] ?? "";
  } else {
    match = normalized.match(new RegExp(`^(${HOUR_WORD_SOURCE})(?:\\s+(${NUMBER_WORD_SOURCE}))?\\s*(a\\.?m\\.?|p\\.?m\\.?)?(?:\\s+(.*))?$`, "i"));
    if (!match) return null;
    hour = spokenNumber(match[1]);
    minute = match[2] ? spokenNumber(match[2]) : 0;
    meridiem = match[3]?.toLowerCase().replaceAll(".", "") ?? null;
    remainder = match[4] ?? "";
  }
  if (minute === null || minute > 59 || hour > (meridiem ? 12 : 23) || hour === 0 && meridiem)
    return { ambiguous: true, reason: "Please give a valid alarm time." };
  const dateQualifier = remainder.trim().toLowerCase();
  if (dateQualifier && dateQualifier !== "today" && dateQualifier !== "tomorrow")
    return { ambiguous: true, reason: DATE_CLARIFICATION };
  if (!meridiem && hour <= 12) return { ambiguous: true, reason: "Please say AM or PM." };
  if (meridiem) hour = hour % 12 + (meridiem === "pm" ? 12 : 0);
  return { hour, minute, dateQualifier };
}

function absoluteAlarm(text, now, timeZone) {
  const tail = text.match(/\b(?:at|for)\s+(.+)$/i);
  if (!tail) return null;
  const parsed = parseAlarmTimeTail(tail[1]);
  if (!parsed || parsed.ambiguous) return parsed;
  const current = zonedParts(now, timeZone);
  const base = new Date(Date.UTC(current.year, current.month - 1,
    current.day + (parsed.dateQualifier === "tomorrow" ? 1 : 0), 12));
  const date = { year: base.getUTCFullYear(), month: base.getUTCMonth() + 1, day: base.getUTCDate(),
    hour: parsed.hour, minute: parsed.minute };
  let epoch = zonedEpoch(date, timeZone);
  if (!parsed.dateQualifier && epoch <= Math.floor(now.getTime() / 1000)) {
    base.setUTCDate(base.getUTCDate() + 1);
    epoch = zonedEpoch({ ...date, year: base.getUTCFullYear(), month: base.getUTCMonth() + 1,
      day: base.getUTCDate() }, timeZone);
  }
  return { epoch };
}

export function parseClockRequest(text, { now = new Date(), timeZone = "Asia/Phnom_Penh" } = {}) {
  let input = String(text ?? "").trim();

  if (/^(?:what(?:'s| is) the time|what time is it|tell me the time|current time)[?.!]*$/i.test(input))
    return { kind: "local", action: "current_time" };
  if (/^(?:what(?:'s| is) today'?s date|what(?:'s| is) the date|what date is it|current date|what day is it)[?.!]*$/i.test(input))
    return { kind: "local", action: "current_date" };

  const timerCreation = /^(?:please\s+)?(?:set|start|create)\s+(?:a\s+)?timer\b/i.test(input) ||
    /^timer\s+for\b/i.test(input);
  if (timerCreation) {
    const seconds = durationSeconds(input);
    return seconds ? { kind: "command", action: "create_timer", duration_s: seconds } :
      { kind: "ambiguous", message: "Please give the timer duration in hours, minutes, or seconds." };
  }

  const alarmCreation = /^(?:please\s+)?(?:set|create)\s+(?:an?\s+)?alarm\b/i.test(input) ||
    /^wake me\b/i.test(input);
  if (alarmCreation) {
    if (UNSUPPORTED_DATE.test(input)) return { kind: "ambiguous", message: DATE_CLARIFICATION };
    const parsed = absoluteAlarm(input, now, timeZone);
    if (!parsed) return { kind: "ambiguous", message: "Please give an alarm time." };
    if (parsed.ambiguous) return { kind: "ambiguous", message: parsed.reason };
    return { kind: "command", action: "create_alarm", epoch_s: parsed.epoch };
  }

  if (/^snooze(?:\s+for\s+.+)?[?.!]*$/i.test(input))
    return { kind: "command", action: "snooze", duration_s: durationSeconds(input) ?? 9 * 60 };
  if (/^(?:dismiss|silence)(?:\s+the)?\s+(?:alarm|timer)[?.!]*$/i.test(input) ||
      /^stop(?:\s+the)?\s+(?:alarm|timer)[?.!]*$/i.test(input) || /^stop[?.!]*$/i.test(input))
    return { kind: "command", action: "dismiss" };
  const cancellation = input.match(/^(?:please\s+)?(?:cancel|delete|remove)\s+(?:(?:my|the)\s+)?(alarm|timer)[?.!]*$/i);
  if (cancellation)
    return { kind: "command", action: "cancel", target: cancellation[1].toLowerCase() };
  if (/^pause(?:\s+the)?\s+timer[?.!]*$/i.test(input)) return { kind: "command", action: "pause_timer" };
  if (/^(?:resume|continue)(?:\s+the)?\s+timer[?.!]*$/i.test(input)) return { kind: "command", action: "resume_timer" };
  if (/^start(?:\s+the)?\s+stopwatch[?.!]*$/i.test(input)) return { kind: "command", action: "start_stopwatch" };
  if (/^pause(?:\s+the)?\s+stopwatch[?.!]*$/i.test(input)) return { kind: "command", action: "pause_stopwatch" };
  if (/^(?:resume|continue)(?:\s+the)?\s+stopwatch[?.!]*$/i.test(input)) return { kind: "command", action: "resume_stopwatch" };
  if (/^(?:reset|clear)(?:\s+the)?\s+stopwatch[?.!]*$/i.test(input)) return { kind: "command", action: "reset_stopwatch" };
  if (/^(?:list|show)(?:\s+(?:my|the|active))?\s+(?:alarms?|timers?|stopwatch)[?.!]*$/i.test(input) ||
      /^(?:alarm|timer|stopwatch)\s+status[?.!]*$/i.test(input) ||
      /^what\s+(?:alarms?|timers?)\s+do\s+i\s+have[?.!]*$/i.test(input) ||
      /^how\s+much\s+time\s+is\s+left\s+on\s+(?:the|my)\s+timer[?.!]*$/i.test(input))
    return { kind: "command", action: "status" };
  return { kind: "not_clock" };
}

export function formatClockReply(request, ack, { now = new Date(), timeZone = "Asia/Phnom_Penh" } = {}) {
  if (!ack?.applied) return ack?.error === "not_found" ? "I couldn't find a matching clock item." : "The clock request was not accepted.";
  if (request.action === "create_timer") return `Timer set for ${formatDuration(request.duration_s)}.`;
  if (request.action === "create_alarm") return `Alarm set for ${new Intl.DateTimeFormat("en-US", { timeZone, weekday: "long", hour: "numeric", minute: "2-digit" }).format(new Date(request.epoch_s * 1000))}.`;
  if (request.action === "current_time") return `It is ${new Intl.DateTimeFormat("en-US", { timeZone, hour: "numeric", minute: "2-digit" }).format(now)}.`;
  if (request.action === "current_date") return `Today is ${new Intl.DateTimeFormat("en-US", { timeZone, weekday: "long", month: "long", day: "numeric", year: "numeric" }).format(now)}.`;
  if (request.action === "status") return ack.summary || "There are no active alarms or timers.";
  return ack.message || "Done.";
}

function formatDuration(seconds) {
  const parts = [];
  if (seconds >= 3600) parts.push(`${Math.floor(seconds / 3600)} hour${seconds >= 7200 ? "s" : ""}`);
  if (seconds % 3600 >= 60) parts.push(`${Math.floor(seconds % 3600 / 60)} minute${Math.floor(seconds % 3600 / 60) === 1 ? "" : "s"}`);
  if (seconds % 60) parts.push(`${seconds % 60} second${seconds % 60 === 1 ? "" : "s"}`);
  return parts.join(" and ");
}

export function clockRequestFromSemantic(
  semantic,
  { now = new Date(), timeZone = "Asia/Phnom_Penh" } = {}
) {
  if (!semantic || semantic.intent === "not_clock")
    return { kind: "not_clock" };

  switch (semantic.intent) {
    case "current_time":
      return { kind: "local", action: "current_time" };

    case "current_date":
      return { kind: "local", action: "current_date" };

    case "create_timer":
      return Number.isInteger(semantic.duration_s) &&
        semantic.duration_s > 0 &&
        semantic.duration_s <= 7 * 24 * 3600
        ? {
            kind: "command",
            action: "create_timer",
            duration_s: semantic.duration_s,
          }
        : {
            kind: "ambiguous",
            message:
              "Please give the timer duration in hours, minutes, or seconds.",
          };

    case "create_alarm": {
      if (semantic.day === "unsupported")
        return {
          kind: "ambiguous",
          message:
            "Please give a supported date such as today or tomorrow.",
        };

      if (
        !Number.isInteger(semantic.hour) ||
        semantic.hour < 1 ||
        semantic.hour > 12
      )
        return {
          kind: "ambiguous",
          message: "Please give an alarm time.",
        };

      if (
        !Number.isInteger(semantic.minute) ||
        semantic.minute < 0 ||
        semantic.minute > 59
      )
        return {
          kind: "ambiguous",
          message: "Please give a valid alarm time.",
        };

      if (semantic.meridiem !== "am" && semantic.meridiem !== "pm")
        return {
          kind: "ambiguous",
          message: "Please say AM or PM.",
        };

      let hour =
        semantic.hour % 12 +
        (semantic.meridiem === "pm" ? 12 : 0);

      const current = zonedParts(now, timeZone);

      const base = new Date(Date.UTC(
        current.year,
        current.month - 1,
        current.day + (semantic.day === "tomorrow" ? 1 : 0),
        12
      ));

      const date = {
        year: base.getUTCFullYear(),
        month: base.getUTCMonth() + 1,
        day: base.getUTCDate(),
        hour,
        minute: semantic.minute,
      };

      let epoch = zonedEpoch(date, timeZone);

      // No explicit day means the next occurrence of that wall-clock time.
      if (
        semantic.day == null &&
        epoch <= Math.floor(now.getTime() / 1000)
      ) {
        base.setUTCDate(base.getUTCDate() + 1);

        epoch = zonedEpoch({
          ...date,
          year: base.getUTCFullYear(),
          month: base.getUTCMonth() + 1,
          day: base.getUTCDate(),
        }, timeZone);
      }

      return {
        kind: "command",
        action: "create_alarm",
        epoch_s: epoch,
      };
    }

    case "cancel":
      return ["alarm", "timer"].includes(semantic.target)
        ? {
            kind: "command",
            action: "cancel",
            target: semantic.target,
          }
        : {
            kind: "ambiguous",
            message: "Please say whether to cancel the alarm or timer.",
          };

    case "dismiss":
      return { kind: "command", action: "dismiss" };

    case "snooze":
      return {
        kind: "command",
        action: "snooze",
        duration_s:
          Number.isInteger(semantic.duration_s) &&
          semantic.duration_s > 0
            ? semantic.duration_s
            : 9 * 60,
      };

    case "pause_timer":
    case "resume_timer":
    case "start_stopwatch":
    case "pause_stopwatch":
    case "resume_stopwatch":
    case "reset_stopwatch":
    case "status":
      return {
        kind: "command",
        action: semantic.intent,
      };

    default:
      return { kind: "not_clock" };
  }
}
