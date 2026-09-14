const destructiveLanguage = /\b(delete|erase|remove|destroy|wipe|reset|revoke|terminate|overwrite)\b/i;

export function classifyAssistantActivity(text, { candidateTools = [] } = {}) {
  const value = String(text ?? "");
  if (candidateTools.length) return "tool_action";
  if (/(?:\d\s*[+*/%-]\s*\d|\b(?:calculate|compute|percent|total)\b)/i.test(value)) return "calculation";
  if (/\b(?:camera|photo|image|look at)\b/i.test(value)) return "camera";
  if (/\b(?:sensor|temperature|humidity|motion|distance|reading|signal)\b/i.test(value)) return "sensor_fusion";
  if (/\b(?:error|failed|broken|debug|diagnose|why.*(?:not|fail))\b/i.test(value)) return "troubleshooting";
  if (/\b(?:remember|memory|previous|last time|what did i)\b/i.test(value)) return "memory";
  if (/\b(?:search|find|look up|latest|current news)\b/i.test(value)) return "search";
  return "conversation";
}

export function routeAssistantRequest({ text, candidateTools = [], context = {} } = {}) {
  const value = String(text ?? "").trim();
  const signals = [];
  const hints = candidateTools.map(tool => tool.annotations ?? {});
  if (candidateTools.some(tool => tool.missingRequired?.length)) signals.push("missing_required_arguments");
  if (context.conflictingEvidence === true || /\b(?:however|conflict(?:ing)?|contradict(?:s|ory)?)\b/i.test(value) ||
      /\b\w+\s+says\b[\s\S]+\b(?:but|while|and)\b[\s\S]+\b\w+\s+says\b/i.test(value)) signals.push("conflicting_evidence");
  if (context.hasHistory === false && /\b(?:that|it|this|them)\b/i.test(value) && value.split(/\s+/).length < 8) signals.push("unresolved_reference");
  if (candidateTools.some(tool => tool.dependsOnPrevious) || context.sequential === true) signals.push("dependent_tool_steps");
  if (candidateTools.length > 1 || context.parallel === true) signals.push("multiple_tool_candidates");
  if ((value.match(/[,;:]|\b(?:then|after|before|using the result)\b/gi) ?? []).length >= 2) signals.push("multi_step_structure");
  if (candidateTools.length && hints.some(hint => hint.readOnlyHint !== true)) signals.push("mutating_tool_hint");
  if (candidateTools.length && hints.some(hint => hint.destructiveHint !== false)) signals.push("destructive_tool_hint");
  if (candidateTools.length && hints.some(hint => hint.openWorldHint !== false)) signals.push("open_world_tool_hint");
  if (destructiveLanguage.test(value)) signals.push("consequential_action_language");
  if (context.uncertain === true) signals.push("upstream_uncertainty");
  return { route: signals.length ? "THINK" : "FAST", reasons: signals.length ? signals : ["low_risk_single_step"],
    activity: classifyAssistantActivity(value, { candidateTools }) };
}

export function authorizeToolInvocation({ tool, hostAuthorized = false } = {}) {
  const hint = tool?.annotations ?? {};
  const potentiallyDestructive = hint.readOnlyHint !== true && hint.destructiveHint !== false;
  return { allowed: !potentiallyDestructive || hostAuthorized === true,
    reason: potentiallyDestructive && !hostAuthorized ? "host_authorization_required" : "authorized",
    hintsTrustedAsAuthorization: false };
}

const feedback = Object.freeze({
  calculation: Object.freeze(["Working through the calculation.", "Checking the numbers."]),
  search: Object.freeze(["Checking the available information.", "Looking through the available sources."]),
  camera: Object.freeze(["Checking the camera.", "Reviewing the camera view."]),
  sensor_fusion: Object.freeze(["Comparing the sensor readings.", "Checking the readings together."]),
  troubleshooting: Object.freeze(["Tracing the problem.", "Checking where the problem starts."]),
  memory: Object.freeze(["Checking what I remember.", "Looking through the relevant memory."]),
  tool_action: Object.freeze(["Checking the requested action.", "Working through the requested action."]),
});

export function progressFeedbackFor({ route, activity, variation = 0 }) {
  if (route !== "THINK" || activity === "conversation") return null;
  const options = feedback[activity] ?? feedback.tool_action;
  return options[Math.abs(Number(variation) || 0) % options.length];
}
