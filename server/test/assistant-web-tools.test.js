import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { createAssistantWebTools, readAgentToolsToken } from "../src/assistant-web-tools.js";

test("agent-tools token is read privately and used only as the HTTP bearer credential", async () => {
  const directory = mkdtempSync(join(tmpdir(), "newo-tools-"));
  const path = join(directory, ".env");
  writeFileSync(path, "OTHER=value\nAGENT_TOOLS_TOKEN=private-caller-token\n", { mode: 0o600 });
  const token = readAgentToolsToken({ envFile: path });
  let request;
  const tools = createAssistantWebTools({ enabled: true, token, fetchImpl: async (url, options) => {
    request = { url, headers: options.headers, body: options.body };
    return new Response(JSON.stringify({ result_count: 0, results: [], elapsed_ms: 3 }), { status: 200 });
  } });
  await tools.invoke("web_search", { query: "test" });
  assert.equal(request.headers.authorization, "Bearer private-caller-token");
  assert.equal(request.body.includes("private-caller-token"), false);
  assert.equal(JSON.stringify(tools.definitions).includes("private-caller-token"), false);
});

test("agent-tools client maps structured timeout errors", async () => {
  const tools = createAssistantWebTools({ enabled: true, token: "private", fetchImpl: async () =>
    new Response(JSON.stringify({ error: { code: "provider_timeout" } }), { status: 504 }) });
  await assert.rejects(tools.invoke("web_search", { query: "test" }), (error) => error.code === "agent_tools_timeout");
});
