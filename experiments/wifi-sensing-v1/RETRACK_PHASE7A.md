# ReTrack Phase 7A foundation

ReTrack is a local-first Linux application for Termux/Android, ordinary Linux,
and later Raspberry Pi or mini-PC collectors. A session does not require the
VPS, Telegram, or internet access. Raw NCSI datagrams are written before DSP or
terminal rendering and remain the authoritative evidence.

## Architecture

`retrackd` is the sole authoritative runtime. `ReTrackCore` owns dynamic node,
topology, geometry, sync, DSP, and recording state. `NetworkRuntime` owns the
one CSI UDP socket, NCOL announcement, ESP lease renewal, and ingestion loop.
The versioned local client server publishes derived snapshots only; it never
copies raw CSI to viewers. Closing every UI leaves tracking and recording
running under `retrackd`.

`retrack run` is only a client. Multiple laptop/Termux viewers may attach to
the same daemon and therefore see the same session ID, counters, nodes, paths,
sync, calibration, and recording state. `retrack replay` feeds either ReTrack
chunks or a legacy Phase-6 `frames.ncsi` archive back into the same
`ReTrackCore.ingest()` and therefore uses the same Phase-5 DSP and Phase-6
synchronization implementation as live collection. Legacy sources are read
in place and are never converted or rewritten.

The package boundaries are `nodes`, `discovery`, `control`, `collector`,
`sessions`, `storage`, `sync`, `dsp`, `replay`, `topology`, `config`, `ui`, and
`export`. The backend registry and topology are dynamically sized. The first UI
shows four slots, but neither storage nor processing assumes four nodes or only
the current three RF paths.

Sessions use an atomically replaced `manifest.json`, append-only
`events.jsonl`, and rotating `frames-NNNNNN.ncsi` NCAP-v2 chunks. Each embedded
NCSI datagram is copied byte-for-byte. A live manifest is `ACTIVE`; clean close
is `COMPLETE`; `retrack recover` marks an interrupted manifest
`INCOMPLETE/RECOVERED` without modifying raw chunks. Replay tolerates a partial
tail only on the last crash-interrupted chunk.

## Local control and ownership

Newo listens on UDP port 5010 while associated with Wi-Fi. ReTrack may discover
the leader with a subnet broadcast and uses compact JSON:

```json
{"protocol":"retrack_control_v1","type":"TRACK_SET","session_id":"...","command_id":1,"state":"ON","lease_ms":15000}
```

The only commands are explicit `TRACK_SET ON`, `TRACK_SET OFF`, and `STATUS`.
The ACK echoes the session and command identities and reports accepted,
duplicate, actual state, owner, remaining lease, node identity, firmware, MAC,
and capabilities. Retransmission uses the exact same command ID and cannot
toggle state. Stale IDs and competing local sessions are rejected.

An accepted local ON establishes an in-memory bounded lease. Commands from the
same session renew it. Explicit OFF releases resources and closes that session;
a later ON uses a new session identity. Lease expiry deterministically invokes
TRACK OFF. ReTrack controls only Newo; the validated Newo-to-Newo2 coordination
remains unchanged.

While a local lease exists, cloud `on`, `off`, and legacy `toggle` transitions
are rejected with `local_session_active`; cloud `status` remains read-only.
After local release or expiry, cloud control works as before. Device reboot
forgets the lease and boots TRACK OFF, while a still-running ReTrack process can
re-establish its same host session with a new command. Collector destination
discovery is independent from control ownership.

This first protocol trusts the local IPv4 subnet. CRC or an echoed identifier is
not authentication. Do not expose UDP 5010 across a routed or untrusted LAN.
Authenticated pairing is deferred; the implementation does not pretend the
current LAN boundary is stronger than it is.

## Daemon client protocol

The daemon client API is newline-delimited JSON over TCP, versioned as
`retrack_client_v1`. It binds `127.0.0.1:8765` by default. Set `api_bind` to a
LAN address or `0.0.0.0` only when another trusted LAN device must view it.
This interface is not authenticated; exposing it grants local-LAN users the
ability to request its documented controller takeover rule.

Any number of read-only viewers may subscribe to derived `SNAPSHOT` messages.
One client may hold a 5-120 second controller lease. A deliberate
`ACQUIRE_CONTROL` with `takeover:true` replaces the current controller; normal
acquisition fails with `controller_busy`. Disconnect does not stop tracking,
recording, or release the lease early. A reconnect using the same client ID can
renew it, or another client can wait for expiry or explicitly take it.

Mutations are explicit `TRACK_SET`, `RECORD_SET`, `EVENT`, and
`PLACEMENT_SET`. They require the current lease ID and a bounded `request_id`.
The daemon caches responses, so retransmission of the same client/request pair
does not execute a side effect twice. Individual clients never renew the ESP
lease; only `retrackd` owns UDP 5010 and its renewal lifecycle.

## Existing calibration loading

`calibration_file` points to the existing `newo_csi` schema; ReTrack does not
define another calibration or change thresholds. The exact room, placement,
DSP feature contract, frozen subcarrier indices, and observed CSI geometry
must match. The UI reports `CAL VALID`, `MISSING`, `REJECTED`, `PENDING`, or
`REQUIRED`. Placement changes immediately invalidate the loaded calibration.
`C` remains reserved and does not build a calibration in Phase 7A.

## Install and run

Termux:

```sh
pkg install python git
cd /path/to/newo/experiments/wifi-sensing-v1/tools
python -m pip install -e .
retrackd --config retrack.example.json
# In a second terminal:
retrack --config retrack.example.json run --controller --client-id laptop
```

Normal Linux uses the same final three commands in a Python 3.10+ virtual
environment. Copy `retrack.example.json` to a private configuration path and
pass `--config PATH` for named rooms and persistent storage choices. Defaults
write under `~/retrack-data`, use `~/.config/retrack/nodes.json`, and keep all
VPS publishing disabled.

For a phone viewer, deliberately expose `api_bind` on the trusted LAN, then:

```sh
retrack --config retrack.example.json run --api-host LAPTOP_LAN_IP --client-id phone
```

The phone is a viewer unless `--controller` is explicitly supplied. `Q`
detaches that client only. Use `--take-control` only for an intentional
controller handoff.

Other commands:

```sh
retrack --room bedroom catalog
retrack --room bedroom --placement TONIGHT_FIXED \
  --calibration-file ../calibrations/bedroom-tonight.json replay SESSION_DIR
retrackd --room bedroom --placement TONIGHT_FIXED \
  --calibration-file ../calibrations/bedroom-tonight.json
```

## Phase 7A limitations

- `C` reserves the calibration action and the geometry model supports
  `REPOSITIONING -> SETTLING -> CALIBRATION_REQUIRED -> BUILDING -> READY`, but
  Phase 7B calibration/model work is not started here.
- Node discovery is the Newo leader control response plus identities learned
  from received NCSI records. Full onboarding, credentials, firmware flashing,
  four-node RF scheduling, and authenticated pairing are deferred.
- `export` is an explicit namespace only. No automatic WAN upload occurs while
  tracking or recording.
- The daemon client API and UDP 5010 rely on a trusted LAN. Authenticated
  pairing and encrypted remote access are deferred.
- RF states remain conservative research evidence. ReTrack does not claim
  identity, localization, pose, occupancy, or a detected person.
