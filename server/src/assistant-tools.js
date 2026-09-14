import { authorizeToolInvocation } from "./assistant-routing.js";

const START = "<|tool_call_start|>";
const END = "<|tool_call_end|>";

export function lfmToolDefinitions(tools = []) {
  if (!tools.length) return null;
  const definitions = JSON.stringify(tools.map(({ name, description, parameters }) => ({ name, description, parameters })));
  return `List of tools: ${definitions} Tool-call syntax uses these literal markers: ${START}[function_name(argument_name="value")]${END}. Never omit the start or end marker when calling a tool.`;
}

class NativeCallParser {
  constructor(source) { this.source = source; this.index = 0; }
  error(message) { throw new Error(`assistant_tool_protocol_invalid: ${message} at ${this.index}`); }
  ws() { while (/\s/.test(this.source[this.index] ?? "")) this.index += 1; }
  take(char) { this.ws(); if (this.source[this.index] !== char) this.error(`expected ${char}`); this.index += 1; }
  identifier() {
    this.ws(); const match = /^[A-Za-z_][A-Za-z0-9_]*/.exec(this.source.slice(this.index));
    if (!match) this.error("expected identifier"); this.index += match[0].length; return match[0];
  }
  string() {
    this.ws(); const quote = this.source[this.index]; if (quote !== "'" && quote !== '"') this.error("expected string");
    this.index += 1; let out = "";
    while (this.index < this.source.length) {
      const char = this.source[this.index++];
      if (char === quote) return out;
      if (char !== "\\") { out += char; continue; }
      const escaped = this.source[this.index++];
      const escapes = { n: "\n", r: "\r", t: "\t", "\\": "\\", "'": "'", '"': '"' };
      if (!(escaped in escapes)) this.error("unsupported escape"); out += escapes[escaped];
    }
    this.error("unterminated string");
  }
  value() {
    this.ws(); const char = this.source[this.index];
    if (char === "'" || char === '"') return this.string();
    if (char === "{" || char === "[") {
      const start = this.index; const stack = []; let quote = null;
      for (; this.index < this.source.length; this.index += 1) {
        const current = this.source[this.index];
        if (quote) { if (current === "\\") this.index += 1; else if (current === quote) quote = null; continue; }
        if (current === '"') { quote = current; continue; }
        if (current === "{" || current === "[") stack.push(current === "{" ? "}" : "]");
        else if (current === "}" || current === "]") {
          if (stack.pop() !== current) this.error("mismatched structured value");
          if (!stack.length) { this.index += 1; try { return JSON.parse(this.source.slice(start, this.index)); } catch { this.error("invalid JSON value"); } }
        }
      }
      this.error("unterminated structured value");
    }
    const match = /^(?:-?\d+(?:\.\d+)?|true|false|null)/.exec(this.source.slice(this.index));
    if (!match) this.error("unsupported argument value"); this.index += match[0].length;
    return match[0] === "true" ? true : match[0] === "false" ? false : match[0] === "null" ? null : Number(match[0]);
  }
  call() {
    const name = this.identifier(); this.take("("); const argumentsValue = {}; this.ws();
    while (this.source[this.index] !== ")") {
      const key = this.identifier(); this.take("=");
      if (Object.hasOwn(argumentsValue, key)) this.error(`duplicate argument ${key}`);
      argumentsValue[key] = this.value(); this.ws();
      if (this.source[this.index] === ",") { this.index += 1; continue; }
      if (this.source[this.index] !== ")") this.error("expected comma or closing parenthesis");
    }
    this.index += 1; return { name, arguments: argumentsValue };
  }
  parse() {
    const calls = []; this.take("["); this.ws();
    while (this.source[this.index] !== "]") {
      calls.push(this.call()); if (calls.length > 8) this.error("too many calls"); this.ws();
      if (this.source[this.index] === ",") { this.index += 1; continue; }
      if (this.source[this.index] !== "]") this.error("expected comma or closing bracket");
    }
    this.index += 1; this.ws(); if (this.index !== this.source.length) this.error("trailing content"); return calls;
  }
}

export function parseLfmToolCalls(text) {
  const value = String(text ?? ""); const start = value.indexOf(START);
  if (start < 0) return { calls: [], content: value, protocol: null };
  const end = value.indexOf(END, start + START.length);
  if (end < 0 || value.indexOf(START, start + START.length) >= 0) throw new Error("assistant_tool_protocol_invalid: unterminated or repeated envelope");
  const calls = new NativeCallParser(value.slice(start + START.length, end).trim()).parse();
  return { calls, content: `${value.slice(0, start)}${value.slice(end + END.length)}`.trim(), protocol: "lfm_native" };
}

function validateType(value, schema = {}) {
  if (!schema.type) return true;
  if (schema.type === "integer") return Number.isInteger(value);
  if (schema.type === "number") return typeof value === "number" && Number.isFinite(value);
  if (schema.type === "array") return Array.isArray(value);
  if (schema.type === "object") return value != null && typeof value === "object" && !Array.isArray(value);
  if (schema.type === "null") return value === null;
  return typeof value === schema.type;
}

export function validateToolCall(call, tools = []) {
  const tool = tools.find((candidate) => candidate.name === call?.name);
  if (!tool) return { ok: false, error: "unknown_tool" };
  const args = call?.arguments;
  if (!args || typeof args !== "object" || Array.isArray(args)) return { ok: false, error: "invalid_arguments" };
  const schema = tool.parameters ?? {}; const properties = schema.properties ?? {};
  for (const required of schema.required ?? []) if (!Object.hasOwn(args, required)) return { ok: false, error: "missing_required_argument", argument: required };
  for (const [key, value] of Object.entries(args)) {
    if (!Object.hasOwn(properties, key) && schema.additionalProperties === false) return { ok: false, error: "unknown_argument", argument: key };
    if (properties[key] && !validateType(value, properties[key])) return { ok: false, error: "invalid_argument_type", argument: key };
  }
  return { ok: true, tool };
}

export async function executeAssistantToolCalls(calls, { tools = [], hostAuthorize = async () => false } = {}) {
  const results = [];
  for (const call of calls) {
    const validation = validateToolCall(call, tools);
    if (!validation.ok) { results.push({ call, ok: false, error: validation.error, argument: validation.argument }); continue; }
    const tool = validation.tool;
    const hostAuthorized = await hostAuthorize({ tool, call });
    const authorization = authorizeToolInvocation({ tool, hostAuthorized });
    if (!authorization.allowed) { results.push({ call, ok: false, error: authorization.reason }); continue; }
    if (typeof tool.execute !== "function") { results.push({ call, ok: false, error: "tool_unavailable" }); continue; }
    try { results.push({ call, ok: true, value: await tool.execute(call.arguments) }); }
    catch (error) { results.push({ call, ok: false, error: error?.code ?? "tool_execution_failed" }); }
  }
  return results;
}
