# ReTrack Phase 7A physical validation

Date: 2026-09-12 local / 2026-09-11 UTC. Branch base was validated Phase-6
`6cddd07f6bcc6c4cc2022b122ed28af0fe6cbe96`.

## Firmware gate and flash

- Newo: COM7, ESP32-S3 revision 0.2, station MAC `7c:4f:ad:2b:3c:68`.
- Newo2 was not connected by USB and its already validated Phase-6 follower was
  not reflashed or modified. Its station MAC remained `28:84:85:4a:2c:c4`.
- Build: Arduino-ESP32 3.3.10, `esp_sr_16`, 16 MB flash, OPI PSRAM, peer MAC
  `28:84:85:4a:2c:c4`.
- Sketch: 2,918,699 bytes; globals: 94,608 bytes; reported dynamic headroom:
  233,072 bytes. Growth from the Phase-6 reference was 8,491 sketch bytes and
  344 global bytes.
- App binary: 2,918,848 bytes, SHA-256
  `b04b3464484cdbeab9ca556924f4f9f3915412fff3cc41006492f90f34d59d5c`.
- The live partition-table readback matched the build partition SHA-256
  `3d0346a98975f252eae4e034b3b3dfd2bda4d97ab4d85ea0eab9ca6c730213a2`.
- Only the app at `0x10000` was written. Bootloader, partition table, NVS,
  calibration, and other partitions were not written or erased.

Boot evidence showed saved Wi-Fi loaded, Wi-Fi associated, display/eyes active,
cloud authenticated, USB host clients ready, 7 MB free PSRAM at startup,
TRACK_OFF resources released, and ReTrack UDP control ready on port 5010. No
panic, reboot loop, watchdog warning, or NVS/provisioning loss was observed.

## Local-only run

Artifacts are outside git at
`C:/Users/re_Lax/Desktop/re/newo-recovery/phase7a-physical-validation-20260912T001800Z`.
The completed session is `20260911T172143Z-6339fb36`.

1. Subnet discovery resolved Newo at `192.168.1.58` with the expected MAC,
   firmware 0.6, and declared capabilities.
2. Explicit `TRACK_SET ON` was ACKed with actual ON and owner LOCAL.
3. Newo2 appeared at `192.168.1.244`; all three current links were received.
4. Phase-6 event-time synchronization reached SYNC_VALID. This is one-way
   event alignment only, not RF phase coherence.
5. Local recording wrote one 997,388-byte NCAP-v2 chunk containing 1,955 exact
   NCSI datagrams. The manifest finalized COMPLETE.
6. `VALIDATION_MARKER` was appended with explicit host monotonic and elapsed
   timestamps between recording start and stop.
7. Explicit `TRACK_SET OFF` was ACKed with actual OFF and owner NONE. A separate
   fresh STATUS confirmed OFF/NONE after the run.
8. `retrack replay` drove the shared pipeline and reproduced all three path
   identities and SYNC_VALID.

End-of-recording descriptive rates were 26.7 Hz ROUTER_NEWO, 36.8 Hz
ROUTER_NEWO2, and 12.0 Hz NEWO2_NEWO. All reported signal quality GOOD in that
short interval. No calibration was loaded, so scores remained unavailable and
states remained LOW_CONFIDENCE; no motion/person/localization claim is made.

The Windows WAN route was not disabled because doing so would disturb unrelated
services. The run instantiated no publisher, reported Publisher DISABLED, and
the collection/control/storage/replay code paths contain only local UDP and
local filesystem operations. Newo's independent cloud connection remained
online but did not participate in any validation step.
