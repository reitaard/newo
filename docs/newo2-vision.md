# Newo2 camera / vision module

This document is the working state for the GOOUUU ESP32-S3-CAM board being evaluated as **Newo2**, the camera/vision sidecar for Newo.

## Architecture status — 2026-09-03

The two-board split remains the intended architecture:

- **Newo** owns microphone/voice, display, speaker/TTS, user-facing state, and the existing authenticated assistant path.
- **Newo2** is the candidate camera/vision/storage board: OV3660 capture, local detection, local identity recognition if the hardware proves capable enough, onboard microSD, snapshots, and vision events.
- Raw camera frames must not be relayed through the original Newo ESP merely to reach the VPS.

The final personalized-Newo2 product role is **not locked yet**. Face recognition at realistic room distance is now the product gate. If Newo2 can reliably distinguish the enrolled user from another person, continue toward a personalized camera assistant. If it cannot, keep the validated camera platform but reconsider the product as a room/door/scene observer where identity is not a hard requirement.

## Physically validated v6 camera baseline

Stable baseline:

```text
branch: goouuu-vision-v6-stable
app version: 6f83166
ESP-IDF: 5.5.5
camera: OV3660, PID 0x3660
source: native JPEG, VGA 640x480, quality 12
frame buffers: 2 x 128 KiB in PSRAM
camera PSRAM DMA: OFF
```

v6 removed the failed RANGE/tiled mode. ACCURATE uses ESPDet-PICO-224 on a full VGA decode; FAST uses MSR+MNP on a 320x240 decode.

### ACCURATE physical result

A real room test produced the following 100-sample ACCURATE window:

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

The earlier RANGE experiment produced only 29.4% hit rate in its comparable room test, required four detector passes, and also produced a non-face detection in another tile. RANGE is rejected.

### Detector threshold decision

Physical testing compared 0.20, 0.30, and 0.40. The user validated **0.30 as the current detector sweet spot**: it can lose a moving face for a frame but reacquires quickly as the person moves through the room, while the troublesome recurring non-face detections were not observed in the chosen camera position. This is the baseline threshold for the recognition product-gate firmware.

This does not mean identity recognition is proven. A roughly 40 x 47 px detected face may still contain too little identity information for reliable local recognition. That is the specific question the next firmware tests.

## v6 stability result

The v6 baseline completed a continuous physical soak lasting at least **2,796,132 ms (~46.6 minutes)** without a reboot. During the same boot it repeatedly switched between FAST and ACCURATE and re-established browser/Wi-Fi sessions.

Observed PASS conditions:

- 16 MB QIO flash and 8 MB octal PSRAM detected correctly;
- both 128 KiB camera frame buffers allocated in PSRAM;
- OV3660 initialized correctly;
- no task-watchdog event/reset;
- no `FB-OVF`;
- no panic, Guru Meditation, abort, brownout, reboot loop, or allocation failure;
- no logged `JPEG decode failed` or `camera capture failed` during the supplied soak.

Three camera framing warnings occurred early in the same boot:

```text
cam_hal: NO-EOI - JPEG end marker missing
cam_hal: NO-SOI - JPEG start marker missing
cam_hal: NO-SOI - JPEG start marker missing
```

No additional SOI/EOI warning appears later through ~46.6 minutes uptime. v6 validates SOI/EOI before publishing a frame and recovered normally. HTTP socket error 104 entries coincided with MJPEG browser disconnect/reconnect. Two Wi-Fi `CCMP replay detected` messages appeared immediately after a station reconnect and did not stop operation.

The v6 camera/detector acceptance soak is therefore **PASS** for proceeding to the recognition product gate.

## Face-recognition product gate

Test branch:

```text
branch: goouuu-face-recognition-test
firmware: recognition-v1
base camera pipeline: v6 stable
source: OV3660 VGA native JPEG
primary detector: ESPDet-PICO-224
fixed detector threshold: 0.30
recognizer: MFN_S8_V1
recognition threshold default: 0.50
max enrollment samples: 5
persistence: RAM only for this test
```

The firmware keeps the proven v6 camera path intact and adds the smallest useful recognition experiment:

```text
OV3660 native VGA JPEG
  -> sanitized shared JPEG frame
  -> RGB565BE VGA decode
  -> ESPDet-224 @ 0.30
  -> choose largest detected face
  -> require 5 facial landmarks / 10 landmark coordinates
  -> MFN_S8_V1 aligned face feature
  -> normalized embedding
  -> dot-product similarity against enrolled samples
  -> ME / UNKNOWN
```

This first benchmark deliberately recognizes only the largest face. Multi-person identity tracking is deferred until single-person recognition proves the optics/model are good enough.

### Build result

CI on ESP-IDF 5.5.5 passes with:

```text
human_face_detect      0.5.0
human_face_recognition 0.3.2
esp-dl                 3.3.11
esp32-camera           2.1.7
esp_jpeg               1.3.1
esp_new_jpeg           1.0.2
```

The published `human_face_recognition 0.3.2` registry metadata still declares an older detector range while its current upstream source targets the newer detector family. The test branch uses ESP-IDF Component Manager's explicit dependency override to retain the physically validated `human_face_detect 0.5.0` rather than downgrading ESPDet. This is deliberate and documented in `main/idf_component.yml`.

CI model packages confirm that only the relevant product-gate models are included:

```text
ESPDet-PICO-224 face detector
MFN_S8_V1 face feature model
```

The compiled application is about 4.60 MB and the merged flash image about 4.66 MB, leaving substantial room in the 8 MB application partition. Runtime PSRAM headroom still must be measured physically after MFN loads; compile/flash fit is not proof of runtime memory fitness.

## Recognition benchmark procedure

Enrollment is intentionally volatile for this test. It disappears after reset because SD persistence would add complexity without answering the product question.

1. Put only the intended user in frame. Enroll 3-5 samples: front, slight left, slight right, and modest natural variation. Do not use only an unrealistically close face.
2. Leave recognition threshold at **0.50** initially.
3. Select **Test: ME** and measure at approximately 1 m, 2 m, 3 m, and the real room position. Test standing still, walking across the frame, slight head turns, leaving the frame, and returning.
4. Record face pixel size, similarity max/average, recognition latency, face-frame count, recognition attempts, and correct-classification percentage.
5. Put a different real person in frame alone, select **Test: OTHER**, and repeat. The critical failure is a false `ME` classification.
6. Only adjust the recognition threshold after observing the similarity separation between the enrolled user and another person. A stable gap is more important than making one screenshot pass.

The web UI counts detection misses separately through `face_frames` and `recognition_attempts`, while its end-to-end product score uses all test frames. That prevents a weak detector from looking like a strong recognizer simply because difficult frames were skipped.

### Product decision rule

```text
Reliable ME at useful room distance + rejects OTHER
  -> personalized Newo2 path survives product gate

Reliable only near the camera
  -> personalized Newo2 remains possible, but placement is constrained

ESPDet detects room-range faces but MFN is frequently UNKNOWN/unstable
  -> conversational camera vision remains viable, but room-wide local identity is not trusted

Poor identity separation even at useful distance
  -> do not build identity-centric production firmware; pivot Newo2 toward room/door/scene observation
```

## If recognition passes

Do not run MFN on every production frame. The benchmark does this only to gather statistics. Production should become event-driven:

```text
ESPDet detects/reacquires face
  -> recognize on acquisition or ambiguity
  -> cache identity for the active track
  -> tolerate short detector misses
  -> rerun recognition after reappearance / identity uncertainty / bounded verification interval
```

Then add a versioned SD face database with 3-5 varied embeddings per person, load active embeddings into RAM/PSRAM at boot, and keep SD reads out of the recognition hot path.

## Conversational vision if product gate passes

A later Vision Turn should work as:

```text
user speech
  -> Newo microphone / existing ASR path
  -> VPS decides the turn needs a fresh image
  -> VPS requests one correlated snapshot from Newo2
  -> Newo2 sends one native JPEG directly to the VPS
  -> multimodal model receives transcript + image
  -> answer returns through existing Newo TTS/speaker path
```

Face identity and generic image understanding are separate capabilities: questions such as "what am I holding?" can use a requested cloud image even if local MFN identity is not available. However, personalized room behavior that depends on knowing **who** is present must not be promised unless this local recognition gate succeeds.

Continuous cloud video remains out of scope.

## Newo <-> Newo2 boundary

If the personalized architecture proceeds, the two ESPs should exchange small state/control messages only. Candidate events include `person_present`, `person_gone`, `identity_known`, `identity_unknown`, `identity_changed`, `camera_error`, and `snapshot_ready`. Candidate commands include `vision_status`, `vision_enable`, `vision_disable`, `capture_snapshot`, and `recognize_now`.

Local UDP remains the first candidate for low-latency state/events while both devices share Wi-Fi; important commands should carry a sequence/request ID with ACK/retry. ESP-NOW remains a later router-independent option. Raw images should go directly between Newo2 and the VPS when requested, not through the original Newo ESP.

## Storage role if Newo2 proceeds

The onboard microSD is already physically validated. Intended production contents remain:

```text
/newo2/faces/        versioned face database / metadata
/newo2/snapshots/    bounded diagnostic/event snapshots
/newo2/logs/         optional bounded persistent diagnostics
/newo2/config/       future local vision configuration if needed
```

Retention must be quota/rotation bounded.

## Current gate state

```text
OV3660 + camera pin map          LOCKED / physically validated
microSD pin map                  LOCKED / physically validated
RANGE tiled detector             REJECTED
Native JPEG VGA capture          ACCEPTED baseline
ESPDet-224 detector              ACCEPTED primary detector
Detector threshold               ACCEPTED baseline: 0.30
v6 stability soak                PASS (~46.6 min supplied log)
MFN recognition firmware         BUILT / CI PASS / physical test pending
Room-range identity separation   PRODUCT GATE — PENDING
Personalized Newo2 product role  NOT LOCKED until recognition result
Vision Turn / VPS integration    DEFERRED until product-gate result
SD face database                 DEFERRED until product-gate result
Production Newo2 firmware        NOT LOCKED
```
