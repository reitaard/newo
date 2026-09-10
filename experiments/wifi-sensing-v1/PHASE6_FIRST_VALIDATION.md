# First Phase 6 physical validation

This procedure is prepared, not performed. It requires explicit flash and VPS
deployment authorization plus verified board identities.

Firmware inputs:

- Newo2 follower: `experiments/wifi-sensing-v1/newo2-node` ESP-IDF target.
- Newo leader: `Newo/Newo.ino` combined Arduino development target, built with
  `NEWO_TRACK_PEER_MAC` set locally to the Newo2 station MAC.
- `experiments/wifi-sensing-v1/newo-rx` is a build-checked standalone receiver,
  not an additional board image for this two-node run.

One-run validation sequence:

1. Before flashing, identify each ESP32-S3 and preserve the existing recovery
   artifacts. Record build SHA, image sizes, AP/BSSID/channel and station MACs.
2. Boot both development images with `TRACK_OFF`. Verify Newo display/RoboEyes,
   cloud hello/status, one voice turn, one speaker playback and available USB
   MSC/VCP/UAC behavior. Record free/min heap, PSRAM and task stack diagnostics.
3. Start `python -m newo_csi field --room bedroom` in Termux. Observe NCOL
   source change to `discovered`; no sync validity is expected while tracking is
   off.
4. Send `/track on`. Require the correlated Telegram ACK to report actual
   ACTIVE and Newo2 active. In the field console require all three named RF
   paths to have nonzero measured Hz and inspect packet/sequence loss; do not
   interpret RF score as a person or location.
5. Record the SYNC state progression, accepted/rejected counts, offset,
   residual distribution, jitter and drift availability. Stable means repeated
   fresh `SYNC_VALID` records with one unchanged leader/follower session pair;
   no millisecond accuracy bound is asserted before this measurement.
6. Reboot Newo. Confirm a new leader session, Newo2 warmup reset, no acceptance
   of the retired session, server desired ON retry with bounded/observable
   backoff, correlated ACTIVE ACK, and return of all paths.
7. Reboot Newo2. Confirm a new follower session, warmup reset, later ACTIVE peer
   refresh without manually rebooting Newo, and new NCSI SYNC epoch.
8. Stop Termux long enough for NCOL lease expiry, then restart it. Confirm CSI
   stays ACTIVE, destination falls back then returns to discovered, raw sequence
   loss is reported, and leader session does not change with collector choice.
9. Interrupt/recover the AP. Confirm Newo safely releases tracking on BSSID loss,
   cloud reconnect creates a fresh command epoch, desired ON reconciliation
   resumes, peer control recovers, and sync warms under a new valid radio epoch.
10. Repeat voice, speaker, display and practical USB operations while ACTIVE.
    Compare cloud latency, audio underruns, USB errors, heap/PSRAM, stack
    headroom, CSI ring high-water/drops, transport drops and ESP-NOW routing
    counters against step 2; record measurements without performance claims.
11. Send `/track off`. Require actual OFF ACK, then verify CSI callback growth
    stops, sender/peer tasks exit, discovery socket closes, ESP-NOW ownership is
    released, Newo2 stops measurement resources, and ordinary Newo functions
    remain available.
12. Preserve the exact `frames.ncsi`, metadata/events and derived Phase-6 report
    outside Git. Verify replay produces the same sync summary and explicitly
    marks unsynchronized intervals.

Hardware gates are ESP-NOW beacon delivery on the real AP channel, one-way
offset/jitter/drift distributions, reboot-session rejection, radio airtime
impact, and combined cloud/audio/display/USB resource behavior. Synthetic and
build tests cannot close these gates.
