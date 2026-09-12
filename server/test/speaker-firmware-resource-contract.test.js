import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const speakerPath = new URL("../../Newo/newo_speaker.cpp", import.meta.url);
const speakerHeaderPath = new URL("../../Newo/newo_speaker.h", import.meta.url);

test("Opus decoder uses a bounded PSRAM task stack with matching deletion", async () => {
  const speaker = await readFile(speakerPath, "utf8");

  assert.match(speaker, /xTaskCreatePinnedToCoreWithCaps\([\s\S]*?SPEAKER_OPUS_DECODER_STACK_BYTES[\s\S]*?MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT\)/);
  assert.match(speaker, /void NewoSpeaker::opusDecoderTask\(\)[\s\S]*?vTaskDeleteWithCaps\(nullptr\);/);
  assert.doesNotMatch(speaker, /xTaskCreatePinnedToCore\(decoderTaskEntry/);
});

test("speaker PCM buffer has PSRAM storage and object-owned static control", async () => {
  const [speaker, header] = await Promise.all([
    readFile(speakerPath, "utf8"),
    readFile(speakerHeaderPath, "utf8"),
  ]);

  assert.match(header, /StaticStreamBuffer_t bufferControl_\s*=\s*\{\};/);
  assert.match(header, /uint8_t\* bufferStorage_\s*=\s*nullptr;/);
  assert.match(speaker, /SPEAKER_BUFFER_BYTES\s*\+\s*1[\s\S]*?heap_caps_malloc\([\s\S]*?MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT[\s\S]*?xStreamBufferCreateStatic/);
  assert.match(speaker, /vStreamBufferDelete\(buffer_\);[\s\S]*?buffer_\s*=\s*nullptr;[\s\S]*?heap_caps_free\(bufferStorage_\);[\s\S]*?bufferStorage_\s*=\s*nullptr;/);
  assert.doesNotMatch(speaker, /xStreamBufferCreate\(/);
});

test("speaker playback task uses a PSRAM stack and logs DMA memory before I2S", async () => {
  const speaker = await readFile(speakerPath, "utf8");

  assert.match(speaker, /xTaskCreatePinnedToCoreWithCaps\([\s\S]*?taskEntry,\s*"newo-speaker",\s*8192[\s\S]*?MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT/);
  assert.match(speaker, /void NewoSpeaker::playbackTask\(\)[\s\S]*?"SPEAKER_I2S_MEMORY"[\s\S]*?i2s_\.begin/);
  assert.match(speaker, /void NewoSpeaker::playbackTask\(\)[\s\S]*?vTaskDeleteWithCaps\(nullptr\);/);
  assert.doesNotMatch(speaker, /xTaskCreatePinnedToCore\(taskEntry/);
});
