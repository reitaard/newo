# Track integration validation

This branch is source/build ready for a development-only physical test. It has
not been flashed or hardware validated. Tracking means RF measurement resources
only; it does not make activity, occupancy, identity, or location claims.

## Development configuration

Build real Newo with `NEWO_TRACK_PEER_MAC` set to the standalone Newo2 station
MAC. The value is a local build setting and is not stored in Wi-Fi/NVS. The
configured collector fallback remains `192.168.1.116:5005`; a live desktop or
Termux collector supersedes it after two matching NCOL announcements and the
node returns to fallback after lease expiry.

`/track`, `/track on`, `/track off`, and `/track status` use correlated cloud
requests. Newo boots `TRACK_OFF`, rejects stale same-session sequences, replays
an exact duplicate result, and coordinates Newo2 over a CRC-protected ESP-NOW
session/sequence control ACK. ON requires the peer ACK. OFF always releases
local resources and reports the peer as stopped or uncertain separately.

The combined receiver uses the validated NCSI serializer and path semantics:
the associated AP BSSID is `ROUTER_NEWO` (path 1), the configured Newo2 station
MAC is `NEWO2_NEWO` (path 3), and unrelated sources are rejected. Router frames
are intentionally ungated as in the hardware-validation receiver; only peer
frames use the configured per-path cadence gate. Signed I/Q bytes, RX metadata,
destination/source MACs, and first-word-invalid sanitization flags are retained.

## First physical validation (not performed)

1. Flash the standalone Newo2 experiment target and the combined development
   Newo build only after explicit authorization and verified board identity.
2. Boot both with tracking OFF. Record cloud/voice/speaker/display/USB behavior,
   free/minimum heap, PSRAM, Wi-Fi stability, and normal task stack headroom.
3. Start `python -m newo_csi field` on the chosen laptop or Termux phone. Verify
   two NCOL announcements select `discovered`, then stop it and verify lease
   expiry returns to `configured` without leaving `TRACK_ACTIVE`.
4. Run `/track status`, `/track on`, duplicate ON, stale-control injection, then
   `/track off`. Confirm each Telegram response follows the matching ACK and
   Newo2 starts/stops its CSI/probe resources with Newo.
5. While ACTIVE, repeat voice capture, speaker streaming, display animation,
   cloud reconnect, provisioning recovery, and sustained USB MSC/VCP/UAC work.
   Compare heap, stack, ring high-water/drops, callback count, transport drops,
   path rates, ESP-NOW state, Wi-Fi disconnects, audio underruns, and USB errors
   against the OFF baseline.
6. Exercise collector absent/reappearing, Newo2 absent/rebooted, AP reconnect or
   channel change, repeated ON/OFF, and reboot during each state. Do not tune DSP
   thresholds from this coexistence test.

## Remaining physical risks

Source builds cannot establish ESP-NOW reliability on the AP channel, multicast
forwarding on the actual router/phone, radio airtime impact, callback cadence,
or whether CSI/ESP-NOW increases cloud latency, voice loss, speaker underruns,
USB instability, task starvation, heap fragmentation, or thermal/power load.
Those measurements gate any production merge.
