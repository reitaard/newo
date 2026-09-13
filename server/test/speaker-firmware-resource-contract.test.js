import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const speakerPath = new URL("../../Newo/newo_speaker.cpp", import.meta.url);
const speakerHeaderPath = new URL("../../Newo/newo_speaker.h", import.meta.url);
const speakerConfigPath = new URL("../../Newo/newo_config.h", import.meta.url);
const audioPath = new URL("../../Newo/newo_audio.cpp", import.meta.url);
const wakeEnginePath = new URL("../../Newo/newo_wake_engine.cpp", import.meta.url);
const serverIndexPath = new URL("../src/index.js", import.meta.url);

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

test("speaker receipt accounting follows successful PCM admission and has a bounded report deadline", async () => {
  const [speaker, header] = await Promise.all([
    readFile(speakerPath, "utf8"),
    readFile(speakerHeaderPath, "utf8"),
  ]);

  assert.match(speaker, /xStreamBufferSend\(buffer_, payload, length, 0\)[\s\S]*?receivedBytes_ \+= length;[\s\S]*?receiptReportPending_ = true;/);
  assert.doesNotMatch(speaker, /receivedBytes_ \+= length;[\s\S]{0,300}xStreamBufferSend\(buffer_, payload, length, 0\)/);
  assert.match(speaker, /newoSpeakerReceiptReportDue\([\s\S]*?SPEAKER_RECEIPT_REPORT_MAX_LATENCY_MS/);
  assert.match(header, /bool receiptReportPending_ = false;/);
});

test("Opus continuity diagnostics sample only active playback and preserve fixed memory bounds", async () => {
  const [speaker, header, config] = await Promise.all([
    readFile(speakerPath, "utf8"),
    readFile(speakerHeaderPath, "utf8"),
    readFile(speakerConfigPath, "utf8"),
  ]);

  assert.match(speaker, /request_\.codec == Codec::OPUS && playbackStarted_ && !endReceived_/);
  assert.match(speaker, /opusReservoirMinimumActiveBytes_/);
  assert.match(speaker, /opusLowReservoir600MsEvents_/);
  assert.match(speaker, /opusLowReservoir400MsEvents_/);
  assert.match(speaker, /opusLowReservoir200MsEvents_/);
  assert.match(header, /uint32_t opusQueueMinimumActivePackets_/);
  assert.match(config, /SPEAKER_OPUS_QUEUE_DEPTH\s*=\s*32/);
  assert.match(config, /SPEAKER_OPUS_STARTUP_PACKETS\s*=\s*25/);
  assert.match(config, /SPEAKER_BUFFER_BYTES\s*=\s*24'576/);
});

test("speaker cancellation is generation-scoped and drops late audio", async () => {
  const [speaker, header] = await Promise.all([
    readFile(speakerPath, "utf8"),
    readFile(speakerHeaderPath, "utf8"),
  ]);
  assert.match(header, /uint32_t generationId;/);
  assert.match(header, /uint32_t lastCancelledGenerationId_/);
  assert.match(speaker, /strcmp\(type, "speaker_cancel"\)/);
  assert.match(speaker, /generationId <= lastCancelledGenerationId_/);
  assert.match(speaker, /decoderAbort_ = true;[\s\S]*?fail\("cancelled"\)/);
  assert.match(speaker, /if \(taskFinished_ \|\| failed_\) return;/);
});

test("a fresh speaker WebSocket resets the server-local cancellation generation", async () => {
  const speaker = await readFile(speakerPath, "utf8");
  const connected = speaker.slice(speaker.indexOf("if (type == WStype_CONNECTED)"), speaker.indexOf("if (type == WStype_BIN)"));
  assert.match(connected, /lastCancelledGenerationId_\s*=\s*0/);
  assert.match(connected, /new ordering epoch/);
});

test("physical barge-in is truthfully disabled while playback suppresses the microphone", async () => {
  const audio = await readFile(audioPath, "utf8");
  const index = await readFile(serverIndexPath, "utf8");
  assert.match(audio, /if \(state_ == NewoVoiceState::STREAMING\) return false/);
  assert.match(audio, /VOICE_MANUAL_BUSY/);
  assert.match(index, /barge_in_available:\s*false/);
});

test("WakeNet is isolated behind the minimal WakeEngine boundary", async () => {
  const audio = await readFile(audioPath, "utf8");
  const wake = await readFile(wakeEnginePath, "utf8");
  assert.match(audio, /wakeEngine_\.start\(i2s_, srEvent\)/);
  assert.match(audio, /wakeEngine_\.stop\(\)/);
  assert.match(wake, /ESP_SR\.begin/);
  assert.doesNotMatch(audio, /ESP_SR\.begin/);
});
