import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

import { createAssistantModeSwitch } from "../src/assistant-mode-switch.js";
import {
  ALFRED_SWITCH_READY,
  ALFRED_SWITCH_START,
  XIAOMEI_SWITCH_READY,
  XIAOMEI_SWITCH_START,
  renderSwitchEarcon,
} from "../src/switch-earcon.js";

function response(status = 200, body = "") {
  return { ok: status >= 200 && status < 300, status, text: async () => body };
}

test("switch earcons are canonical PCM16 and model-distinct", () => {
  const xiaomei = renderSwitchEarcon(XIAOMEI_SWITCH_START);
  const alfred = renderSwitchEarcon(ALFRED_SWITCH_START);
  assert.ok(Buffer.isBuffer(xiaomei));
  assert.ok(Buffer.isBuffer(alfred));
  assert.equal(xiaomei.length % 2, 0);
  assert.equal(alfred.length % 2, 0);
  assert.notDeepEqual(xiaomei, alfred);
  assert.ok(renderSwitchEarcon(XIAOMEI_SWITCH_READY).length < xiaomei.length);
  assert.ok(renderSwitchEarcon(ALFRED_SWITCH_READY).length < alfred.length);
});

test("toggle unloads Alfred, loads Xiaomei, persists, and emits direction earcons", async () => {
  const requests = [];
  const tones = [];
  const persisted = [];
  const manager = createAssistantModeSwitch({
    enabled: true,
    initialMode: "alfred",
    ollamaBaseUrl: "http://ollama",
    alfredModel: "newo-minicpm5:latest",
    xiaomeiTtsBaseUrl: "http://xiaomei",
    fetchImpl: async (url, options) => { requests.push([url, options.body ? JSON.parse(options.body) : null]); return response(); },
    playEarcon: async (tone) => tones.push(tone),
    persistMode: async (mode) => persisted.push(mode),
  });

  const state = await manager.toggle();
  assert.equal(state.mode, "xiaomei");
  assert.deepEqual(tones, [XIAOMEI_SWITCH_START, XIAOMEI_SWITCH_READY]);
  assert.deepEqual(persisted, ["xiaomei"]);
  assert.equal(requests[0][0], "http://ollama/api/generate");
  assert.equal(requests[0][1].keep_alive, 0);
  assert.equal(requests[1][0], "http://xiaomei/admin/load");
});

test("return to Alfred unloads Xiaomei and warms main model forever", async () => {
  const requests = [];
  const manager = createAssistantModeSwitch({
    enabled: true,
    initialMode: "xiaomei",
    ollamaBaseUrl: "http://ollama",
    alfredModel: "newo-minicpm5:latest",
    xiaomeiTtsBaseUrl: "http://xiaomei",
    fetchImpl: async (url, options) => { requests.push([url, options.body ? JSON.parse(options.body) : null]); return response(); },
  });

  const state = await manager.toggle();
  assert.equal(state.mode, "alfred");
  assert.equal(requests[0][0], "http://xiaomei/admin/unload");
  assert.equal(requests[1][0], "http://ollama/api/generate");
  assert.equal(requests[1][1].keep_alive, -1);
});

test("failed Xiaomei load restores Alfred instead of stranding the GPU", async () => {
  const requests = [];
  const persisted = [];
  const manager = createAssistantModeSwitch({
    enabled: true,
    initialMode: "alfred",
    ollamaBaseUrl: "http://ollama",
    alfredModel: "newo-minicpm5:latest",
    xiaomeiTtsBaseUrl: "http://xiaomei",
    fetchImpl: async (url, options) => {
      const body = options.body ? JSON.parse(options.body) : null;
      requests.push([url, body]);
      if (url.endsWith("/admin/load")) return response(500, "load failed");
      return response();
    },
    persistMode: async (mode) => persisted.push(mode),
  });

  await assert.rejects(() => manager.toggle(), /Xiaomei load failed/);
  assert.equal(manager.status().mode, "alfred");
  assert.equal(requests.at(-1)[0], "http://ollama/api/generate");
  assert.equal(requests.at(-1)[1].keep_alive, -1);
  assert.deepEqual(persisted, ["alfred"]);
});

test("Telegram exposes the translator switch only as /translate", async () => {
  const source = await readFile(new URL("../src/telegram-mode-commands.js", import.meta.url), "utf8");
  assert.match(source, /command:\s*"translate"/);
  assert.match(source, /\^\\\/translate/);
  assert.match(source, /Use \/translate to toggle/);
  assert.doesNotMatch(source, /Use \/xiaomei|\^\\\/\(\?:xiaomei|command:\s*"xiaomei"/);
});
