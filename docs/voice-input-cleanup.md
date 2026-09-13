# Voice input cleanup checkpoint

This document is the repository source of truth for the `voice-input-cleanup` work. Older chat/session notes describing a same-task 1 s pre-connect buffer are superseded by this architecture.

## Why the old pre-roll was insufficient

The production firmware pins `WebSockets` 2.7.2. Its ESP32 client performs TCP/TLS connection work synchronously from `WebSocketsClient::loop()` and uses a 5 s TCP timeout. Binary writes can also block the calling transport task.

A microphone read placed after `voiceWebSocket_.loop()` therefore does not protect speech spoken while that call is blocked. A 1 s ring also cannot cover the transport's 5 s blocking ceiling.

## Current streaming architecture

```text
INMP441 / I2S
     |
     v
capture task (core 1, priority 3)
     |
     +-- WebRTC NS medium
     |   AGC disabled
     |
     v
bounded 10 s PSRAM PCM ring
     |
     v
network task (core 0, priority 2)
     |
     +-- TLS / WebSocket connect
     +-- bounded backlog drain, max 100 ms per WS frame
     |
     v
/voice -> Sherpa
```

The capture task never performs WebSocket work. The network task never reads I2S. A TLS/connect/write stall can therefore delay transmission without stopping microphone capture. The ring overwrites oldest frames only after its fixed 10 s capacity is exhausted, keeping latency and memory bounded.

## DSP policy

- Streaming uses the standalone ESP-SR WebRTC noise suppressor at 16 kHz / 20 ms.
- NS mode is `1` (medium).
- AGC is disabled.
- Failure to create/process the configured NS path is an explicit stream failure; production must not silently claim NS while sending raw PCM.
- WakeNet remains on the existing ESP_SR path and is not routed through the streaming NS task.
- AEC is intentionally not part of this checkpoint. Proper AEC requires simultaneous speaker + microphone operation and the exact playback reference signal.

## Sherpa endpoint policy

Defaults:

- rule 1 trailing silence: 2.0 s
- rule 2 trailing silence: 1.0 s
- rule 3 maximum utterance: 20 s

Optional overrides are `VOICE_ASR_ENDPOINT_RULE1_S`, `VOICE_ASR_ENDPOINT_RULE2_S`, and `VOICE_ASR_ENDPOINT_RULE3_S`. Overrides are range-validated and the worker reports the effective values back in `SHERPA_READY`, so telemetry reflects what the recognizer actually uses.

## Required physical validation

Before merge to `main`, compile with the pinned production Arduino stack and then test on Newo hardware. A healthy run should show:

- `VOICE_NS_READY` (not a silent raw fallback)
- `VOICE_PREROLL` with `capture_start_ms` near the beginning of the turn
- `overwritten_frames=0` under normal connection conditions
- `VOICE_PCM_HEALTH` with plausible raw and transmitted RMS/peak values
- no `VOICE_CAPTURE_OVERWRITE` during ordinary turns
- `VOICE_CAPTURE_SUMMARY` with captured/dequeued/sent counts consistent with the session outcome

Test immediate speech after trigger, normal speech after connection, background steady noise, and cancellation. Do not merge solely because host/CI tests pass; the microphone/NS result needs the physical board.
