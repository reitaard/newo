# `/track` and real-Newo integration plan/implementation record

This remains the contract and records the integration-branch implementation.
It does not authorize a firmware merge or flash. `/track` controls RF
measurement, not interpretation, recording, presence, or localization.

## Command and state contract

The only states are `TRACK_OFF` and `TRACK_ACTIVE`; development boots
`TRACK_OFF`. Collector availability is telemetry and never gates
`TRACK_ACTIVE`. Tracking state is volatile across reboot for the first
integration, so every reboot returns OFF and Newo tells Newo2 to stop when the
peer becomes reachable. Persistence may be considered only after resource and
failure testing.

Cloud messages follow the existing correlated device-control pattern:

```text
track_control { request_id, action: toggle|on|off|status, command_epoch, command_sequence }
track_ack     { request_id, applied, state, command_epoch, command_sequence,
                newo2_state, collector_state, error }
```

The VPS accepts `/track`, `/track on`, `/track off`, and `/track status`, maps a
bare command to atomic `toggle`, and replies only after the matching
`track_ack`. Newo, not the VPS, resolves toggle against its current state. Newo
keeps a bounded recent `(epoch, sequence)` result cache: an exact duplicate
replays the prior ACK without reapplying; an older sequence is rejected as
stale. A new authenticated device connection creates a new command epoch.
Queue-full, transition-busy, peer-start failure, and resource-start failure
produce `applied:false`; they never claim success.

Newo owns the authoritative transition. ON allocates local measurement
resources, enables CSI, starts the sender path, commands Newo2, and ACKs ACTIVE
only after local activation and a Newo2 acknowledgement. OFF first prevents new
CSI enqueue, tells Newo2 to stop, unregisters/disables CSI, drains or explicitly
drops the bounded ring, stops sender/probe tasks, closes the UDP socket, removes
the ESP-NOW peer if tracking owns it, and ACKs only after release. Status does
not mutate state. A lost collector leaves callbacks/ring counters operational
and reports `collector_state:unavailable`; it does not turn tracking off.

## Real Newo files for the later implementation

Modify only these existing production-side files:

- `Newo/Newo.ino`: construct `NewoTracking`, call `begin()` after Wi-Fi setup,
  consume queued cloud requests in `loop()`, and send ACKs after transitions.
- `Newo/newo_cloud.h` and `Newo/newo_cloud.cpp`: bounded `TrackRequest` queue,
  strict action/ID/epoch/sequence parsing, and `sendTrackAck()`.
- `Newo/newo_wifi.h` and `Newo/newo_wifi.cpp`: expose association/channel and a
  narrow event-listener hook; keep the existing single `WiFi.onEvent` owner and
  forward relevant connect/disconnect/channel events to tracking.
- `Newo/newo_config.h`: development defaults and bounded ring/task/transport
  constants, with tracking default OFF.
- Add `Newo/newo_tracking.{h,cpp}` as the lifecycle/state owner.
- Add reusable measurement modules under `Newo/newo_csi/` as described below.

Cloud/Telegram changes belong in `server/src/telegram-mode-commands.js` for
parsing and ACK-only replies, `server/src/index.js` for the correlated device
message and reconnect epoch, the command menu if `/track` is made visible, and
focused tests in `server/test/telegram-mode-commands.test.js` plus a new track
contract test. Telegram remains VPS-side; no bot token or Telegram library is
added to firmware.

## Reusable CSI modules

Move/adapt, with host tests retained:

- `ncsi_protocol.c/.h`: versioned serialization only.
- `rate_gate.c/.h`: peer-path scheduling policy only.
- `probe_protocol.c/.h`: Newo-to-Newo2 control/probe frames.
- Extract the standalone app's fixed ring into `newo_csi_ring`.
- Extract nonblocking CSI callback filtering/copy/enqueue into
  `newo_csi_capture`.
- Extract UDP serialization/send/counters into `newo_csi_transport`.
- Extract ESP-NOW peer/control/ACK ownership into `newo_csi_peer`.

Do **not** copy the standalone `app_main`, NVS initialization/erase behavior,
standalone Wi-Fi station bring-up/reconnect loop, hard-coded collector address,
standalone event loop, or task startup order. Do not let CSI callbacks allocate,
serialize, call sockets, log verbosely, or block; they only validate metadata,
copy into a preallocated ring slot, update atomic counters, and return.

## Ownership and coexistence

`NewoTracking` owns state, CSI registration, the fixed ring, sender/probe tasks,
UDP socket, and tracking ESP-NOW peer. `NewoWiFi` remains the sole Wi-Fi
connection/provisioning owner. Newo remains leader and sends idempotent
start/stop commands to Newo2 containing boot/session ID and sequence; Newo2
rejects stale commands and ACKs its applied state. Reassociation invalidates
channel-dependent capture state, rebinds the peer on the new AP channel, and
increments an association epoch without converting TRACK_ACTIVE to OFF.

Startup order is storage/display, Wi-Fi registration, cloud, normal
audio/speaker/USB clients, then dormant tracking initialization. TRACK_ACTIVE
acquisition occurs only on a command. Shutdown order is capture gate, peer stop,
CSI disable/unregister, bounded-ring drain/drop accounting, sender/probe task
join, socket close, ESP-NOW peer release, then state/ACK publication.

The first combined build must measure binary/IRAM/DRAM growth, free/minimum
heap and PSRAM, task stack high-water, ring high-water/drops, send latency and
errno buckets, Wi-Fi disconnects/association epochs, ESP-NOW delivery, and CPU
load at OFF and ACTIVE. Repeat voice streaming, WakeNet/off behavior, display
animation, speaker streaming/underruns, cloud reconnect/status/health/logs,
USB MSC/VCP/UAC enumeration and sustained transfer, provisioning, and clean
tracking on/off cycles. Test collector absent/reappearing, Newo2 absent/rebooted,
AP reconnect/channel change, duplicate/stale commands, queue full, and reboot
during both transitions.

## Gate to implementation

Before touching combined firmware: complete one controlled schema-v3
calibration/replay check, one real 40/60/80-column Termux session, and the
collector-discovery firmware decision needed to select a phone without a
hard-coded laptop address. Then implement behind development-default OFF and
measure coexistence before any production integration.
