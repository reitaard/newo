import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const speakerPath = new URL("../../Newo/newo_speaker.cpp", import.meta.url);

test("Opus decoder uses a bounded PSRAM task stack with matching deletion", async () => {
  const speaker = await readFile(speakerPath, "utf8");

  assert.match(speaker, /xTaskCreatePinnedToCoreWithCaps\([\s\S]*?SPEAKER_OPUS_DECODER_STACK_BYTES[\s\S]*?MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT\)/);
  assert.match(speaker, /void NewoSpeaker::opusDecoderTask\(\)[\s\S]*?vTaskDeleteWithCaps\(nullptr\);/);
  assert.doesNotMatch(speaker, /xTaskCreatePinnedToCore\(decoderTaskEntry/);
});
