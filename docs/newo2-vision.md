# Newo2 camera / vision module

This document is the working state for the GOOUUU ESP32-S3-CAM board that will become **Newo2**, the camera/vision sidecar for Newo.

## Architecture decision — 2026-09-03

The hardware role is now **locked**:

- **Newo** remains the interaction/audio board: microphone, wake/voice lifecycle, display, speaker, user-facing state, and the existing authenticated assistant path.
- **Newo2** becomes the camera/vision/storage board: OV3660 capture, local face detection, future local face recognition, onboard microSD persistence, snapshots, and vision events.
- Newo2 is not a replacement for the existing Newo board and raw camera frames must not be relayed through Newo just to reach the VPS.

The role is locked, but the production Newo2 firmware is not finished. The current `goouuu-vision-v6-stable` firmware is the validated vision baseline from which production integration will be built.

## Physically validated v6 baseline

Physical test firmware:

```text
branch: goouuu-vision-v6-stable
app version: 6f83166
ESP-IDF: 5.5.5
camera: OV3660, PID 0x3660
source: native JPEG, VGA 640x480, quality 12
frame buffers: 2 x 128 KiB in PSRAM
camera PSRAM DMA: OFF
```

The v6 build removed the failed RANGE/tiled mode. It exposes VIEW, FAST, ACCURATE, and BENCH. ACCURATE uses ESPDet-224 on a full VGA decode; FAST uses MSR+MNP on a 320x240 decode.

### ACCURATE physical result

A real room test with a person at room range produced the following 100-sample window:

| Metric | Result |
| --- | ---: |
| Model | ESPDet-224 |
| JPEG decode | 52 ms |
| Detect now | 245 ms |
| Detect average | 246.7 ms |
| Detect p95 | 255 ms |
| AI total | 297 ms |
| AI rate | 3.4 fps |
| Detection hit rate | 98.0% (98/100) |
| Face streak | 98 |
| Faces reported in captured frame | 2 |
| Largest reported face | 40 x 47 px |
| Free PSRAM | 3.01 MB |
| Largest PSRAM block | 2.81 MB |
| Free internal RAM | 65 KB |
| Sensor PID | 0x3660 |

This is a large recall improvement over the earlier RANGE experiment, which produced only 29.4% hit rate in the comparable room test despite doing four detector passes. RANGE also produced at least one non-face detection in a separate tile and is not part of the production direction.

The captured ACCURATE screenshot contains two boxes. The screenshot alone is not enough to prove that both are true faces, so the 98% hit rate must be treated as **presence recall**, not multi-face precision. Empty-room and controlled false-positive tests are still required before fixing the final detector threshold.

The firmware boots at threshold `0.30`. The physical session also exercised `0.40` and `0.20`. No final production threshold has been selected yet.

## Stability result

The v6 baseline completed a continuous physical soak lasting at least **2,796,132 ms (~46.6 minutes)** without a reboot. During that period the same boot instance moved repeatedly between FAST and ACCURATE and re-established browser/Wi-Fi sessions without restarting.

The clean v6 run showed:

- 16 MB QIO flash detected correctly;
- 8 MB octal PSRAM detected and memory-tested successfully;
- both 128 KiB camera frame buffers allocated in PSRAM;
- OV3660 initialized correctly;
- Wi-Fi AP and browser stream started;
- FAST and ACCURATE both ran;
- no task-watchdog event or reset;
- no `FB-OVF` framebuffer overflow;
- no panic, Guru Meditation, abort, brownout, reboot loop, or heap-allocation failure in the supplied log;
- no logged `JPEG decode failed` or `camera capture failed` event during the supplied soak.

Three lower-level camera/JPEG framing warnings occurred early in the same boot, shortly after ACCURATE was first entered:

```text
cam_hal: NO-EOI - JPEG end marker missing
cam_hal: NO-SOI - JPEG start marker missing
cam_hal: NO-SOI - JPEG start marker missing
```

No additional SOI/EOI warning appears later in the supplied log through ~46.6 minutes uptime. v6 validates SOI/EOI before publishing a frame, so malformed frames do not enter the AI/shared-frame path. The camera driver recovered and continued operating. The warning mechanism remains a production-hardening item, but the **v6 acceptance soak is considered PASS** for continuing integration.

HTTP socket error `104` entries coincide with browser MJPEG clients disconnecting/reconnecting and are not treated as camera or inference failures. A pair of Wi-Fi `CCMP replay detected` messages occurred immediately after a station reconnect; the AP remained online and the client resumed normally, so these are recorded as a network observation rather than a vision-pipeline failure.

The earlier measured memory point under ACCURATE was approximately 3.01 MB free PSRAM, 2.81 MB largest PSRAM block, and 65 KB free internal RAM. Long-term production testing should still expose minimum/low-water memory telemetry so leaks can be detected without relying on screenshots.

## Detector decision

**ACCURATE / ESPDet-224 is the primary Newo2 detector candidate.** The measured ~297 ms camera-to-detection AI path is acceptable for presence, identity acquisition, and conversational vision triggers. It is not a 30 FPS tracker and should not be treated as one.

Production should avoid running expensive work simply because a browser is open. The intended behavior is event-driven:

```text
camera capture
  -> local detector
  -> face/person event
  -> recognition only when identity is needed
  -> cache/track result for a short window
  -> rerun recognition on new/reappearing/ambiguous face
```

FAST remains useful as an optional cheaper scout/benchmark path, but the production architecture does not depend on it being the primary detector.

## Newo <-> Newo2 boundary

The two ESPs should exchange **small state/control messages only**. Do not push raw VGA frames, PCM, or model buffers over the inter-board link.

First production transport candidate: local UDP while both devices are on the same 2.4 GHz LAN. Important commands/events should carry a sequence/request ID and use ACK/retry. ESP-NOW remains a later option if a router-independent direct link becomes necessary.

Example local events:

```text
vision_online
vision_offline
person_present
person_gone
face_seen
identity_known
identity_unknown
identity_changed
camera_busy
camera_error
snapshot_ready
```

Example commands from Newo:

```text
vision_status
vision_enable
vision_disable
capture_snapshot
recognize_now
```

The production protocol must be versioned and bounded. It should carry IDs, confidence, bounding-box metadata, timestamps/age, and status; not images.

## Conversational AI path

Newo2 should make camera-based conversation possible without putting continuous cloud vision on the hot path.

For a voice request such as "Neo, what am I holding?":

```text
user speech
  -> Newo microphone
  -> existing /voice ASR path
  -> VPS detects that the turn requires vision
  -> VPS requests one fresh snapshot from Newo2
  -> Newo2 captures/sends one JPEG directly to the VPS vision endpoint
  -> multimodal model receives transcript + image
  -> text answer
  -> existing Newo TTS/speaker path
```

Newo2 should upload the JPEG directly to the VPS rather than relaying image bytes through the original Newo ESP. The original Newo board still owns the spoken conversation and user-facing display state.

Continuous video upload is explicitly out of scope. Cloud images should be requested only for a user turn, an explicitly enabled automation, or a specific diagnostic/event policy.

### Immediate integration slice: Vision Turn v0

Before investing in the complete identity database, build one thin end-to-end camera conversation test:

1. Newo2 joins normal Wi-Fi as a station while keeping the current camera pipeline intact.
2. Newo2 opens its own authenticated outbound VPS connection with a distinct device role such as `newo2-vision`.
3. VPS can send one correlated `vision_capture` request.
4. Newo2 returns one fresh native JPEG directly to the VPS with request ID, capture age, width/height, and byte length.
5. The VPS sends transcript + JPEG to a multimodal model.
6. The answer reuses the already-working Newo speaker/TTS path.

This proves the important product experience — talking to Newo about what the camera sees — without waiting for face recognition. It also proves that image traffic bypasses the original Newo ESP and that the two-board architecture works end to end.

The benchmark SoftAP should remain available only as a debug mode. Production/Vision Turn v0 must not depend on the phone being connected to `NEWO-CAM-TEST`.

## Face recognition path

Face recognition remains the next major **local** vision feature after the first end-to-end Vision Turn v0 is proven.

Planned flow:

```text
ACCURATE detects face
  -> obtain/verify alignment landmarks
  -> align face to recognizer input
  -> MFN_S8_V1 embedding
  -> compare against active embeddings in RAM/PSRAM
  -> known / unknown result
  -> cache identity while the same face remains tracked
```

Before coding the recognizer, verify the exact alignment/landmark path exposed by the current Espressif face components. Do not assume the ESPDet-224 bounding box by itself is sufficient for MFN alignment.

Enrollment target:

- 3-5 varied samples per person;
- embeddings persisted on microSD;
- active database loaded into RAM/PSRAM at boot;
- no SD read in the per-frame recognition hot path;
- explicit learn/list/forget/forget-all operations;
- database format versioned before real identities are stored.

## Storage role

The onboard microSD is already physically validated and remains reserved for Newo2. Intended production contents include:

```text
/newo2/faces/        versioned face database / metadata
/newo2/snapshots/    optional bounded diagnostic/event snapshots
/newo2/logs/         optional bounded persistent diagnostics later
/newo2/config/       future local vision configuration if needed
```

Do not let snapshot/log retention grow without a quota or rotation policy.

## Remaining gates

The next work should be done in this order:

1. **Detector acceptance soak — PASS** — ~46.6 minutes continuous v6 runtime with no watchdog, `FB-OVF`, panic, reboot, logged decode failure, or camera-capture failure. Three early SOI/EOI warnings recovered and did not recur in the supplied log.
2. **Precision test** — empty room plus known non-face objects at thresholds 0.30 and 0.40; choose the lowest threshold that does not create recurring false positives.
3. **Vision Turn v0 transport** — Newo2 Wi-Fi STA + authenticated outbound VPS connection + correlated one-shot JPEG capture/upload.
4. **Assistant vision proof** — transcript + requested snapshot into a multimodal model, then reuse the existing Newo TTS/speaker path.
5. **Recognition benchmark** — add MFN only, measure one recognition operation and memory cost.
6. **Identity cache/tracking** — avoid rerunning MFN every frame.
7. **SD face database** — enroll/list/forget with versioned persistence and active embeddings in memory.
8. **Local Newo protocol** — UDP discovery/heartbeat, status/events, sequence IDs, ACK/retry for low-latency local reactions.
9. **Production hardening** — Newo2 provisioning/recovery, watchdog/soak tests, bounded storage, authentication, OTA/recovery behavior, and removal of benchmark-only UI from the normal hot path.

## Current lock state

```text
Newo2 board role                 LOCKED: camera/vision/storage module
OV3660 + camera pin map          LOCKED / physically validated
microSD pin map                  LOCKED / physically validated
RANGE tiled detector             REJECTED
Native JPEG VGA capture          ACCEPTED baseline
ACCURATE / ESPDet-224            ACCEPTED primary detector candidate
v6 detector stability soak       PASS (~46.6 min supplied log)
Final detector threshold         PENDING precision test
Vision Turn v0                   NEXT integration milestone
Face recognition                 NEXT local-AI milestone after vision-turn proof
Local Newo event link            PLANNED
Production Newo2 firmware        NOT YET LOCKED
```
