const CLOCK_WORDS = /\b(alarm|timer|stopwatch|snooze|clock|time|date)\b/i;

function durationSeconds(text) {
  let total = 0;
  let matched = false;
  const units = { hour: 3600, hours: 3600, minute: 60, minutes: 60, second: 1, seconds: 1 };
  for (const match of text.matchAll(/\b(\d{1,4})\s*(hours?|minutes?|seconds?)\b/gi)) {
    total += Number(match[1]) * units[match[2].toLowerCase()];
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

function absoluteAlarm(text, now, timeZone) {
  const match = text.match(/\b(?:at|for)\s+(\d{1,2})(?::(\d{2}))?\s*(a\.?m\.?|p\.?m\.?)?\b/i);
  if (!match) return null;
  let hour = Number(match[1]);
  const minute = Number(match[2] ?? 0);
  const meridiem = match[3]?.toLowerCase().replaceAll(".", "") ?? null;
  if (minute > 59 || hour > (meridiem ? 12 : 23) || hour === 0 && meridiem) return null;
  if (!meridiem && hour <= 12) return { ambiguous: true, reason: "Please say AM or PM." };
  if (meridiem) hour = hour % 12 + (meridiem === "pm" ? 12 : 0);
  const current = zonedParts(now, timeZone);
  const base = new Date(Date.UTC(current.year, current.month - 1, current.day + (/\btomorrow\b/i.test(text) ? 1 : 0), 12));
  const date = { year: base.getUTCFullYear(), month: base.getUTCMonth() + 1, day: base.getUTCDate(), hour, minute };
  let epoch = zonedEpoch(date, timeZone);
  if (!/\b(today|tomorrow)\b/i.test(text) && epoch <= Math.floor(now.getTime() / 1000)) {
    base.setUTCDate(base.getUTCDate() + 1);
    epoch = zonedEpoch({ ...date, year: base.getUTCFullYear(), month: base.getUTCMonth() + 1, day: base.getUTCDate() }, timeZone);
  }
  return { epoch };
}

export function parseClockRequest(text, { now = new Date(), timeZone = "Asia/Bangkok" } = {}) {
  const input = String(text ?? "").trim();
  const lower = input.toLowerCase();
  if (!CLOCK_WORDS.test(input) && !/^\s*(stop|dismiss|pause|resume)\s*$/i.test(input)) return { kind: "not_clock" };
  if (/\b(what(?:'s| is) the time|current time|time is it)\b/i.test(input)) return { kind: "local", action: "current_time" };
  if (/\b(what(?:'s| is) (?:today'?s )?date|current date|date is it|what day is it)\b/i.test(input)) return { kind: "local", action: "current_date" };
  if (/\b(set|start|create)\b.*\btimer\b|\btimer\b.*\bfor\b/i.test(input)) {
    const seconds = durationSeconds(input);
    return seconds ? { kind: "command", action: "create_timer", duration_s: seconds } :
      { kind: "ambiguous", message: "Please give the timer duration in hours, minutes, or seconds." };
  }
  if (/\b(set|create|wake me)\b.*\balarm\b|\bwake me\b.*\bat\b/i.test(input)) {
    const parsed = absoluteAlarm(input, now, timeZone);
    if (!parsed) return { kind: "ambiguous", message: "Please give an alarm time." };
    if (parsed.ambiguous) return { kind: "ambiguous", message: parsed.reason };
    return { kind: "command", action: "create_alarm", epoch_s: parsed.epoch };
  }
  if (/\bsnooze\b/i.test(input)) return { kind: "command", action: "snooze", duration_s: durationSeconds(input) ?? 9 * 60 };
  if (/\b(dismiss|stop (?:the )?(?:alarm|timer)|silence)\b/i.test(input) || /^stop$/i.test(lower)) return { kind: "command", action: "dismiss" };
  if (/\b(cancel|delete|remove)\b.*\b(timer|alarm)\b/i.test(input)) return { kind: "command", action: "cancel", target: lower.includes("alarm") ? "alarm" : "timer" };
  if (/\bpause\b.*\btimer\b/i.test(input)) return { kind: "command", action: "pause_timer" };
  if (/\b(resume|continue)\b.*\btimer\b/i.test(input)) return { kind: "command", action: "resume_timer" };
  if (/\b(start)\b.*\bstopwatch\b/i.test(input)) return { kind: "command", action: "start_stopwatch" };
  if (/\bpause\b.*\bstopwatch\b/i.test(input)) return { kind: "command", action: "pause_stopwatch" };
  if (/\b(resume|continue)\b.*\bstopwatch\b/i.test(input)) return { kind: "command", action: "resume_stopwatch" };
  if (/\b(reset|clear)\b.*\bstopwatch\b/i.test(input)) return { kind: "command", action: "reset_stopwatch" };
  if (/\b(list|what|show|status|remaining|left)\b.*\b(alarms?|timers?|stopwatch)\b/i.test(input)) return { kind: "command", action: "status" };
  return { kind: "ambiguous", message: "I understood this as a clock request, but I need a clearer action or time." };
}

export function formatClockReply(request, ack, { now = new Date(), timeZone = "Asia/Bangkok" } = {}) {
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
