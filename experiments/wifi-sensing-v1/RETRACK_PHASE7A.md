# ReTrack Phase 7A foundation

ReTrack is a local-first Linux application for Termux/Android, ordinary Linux,
and later Raspberry Pi or mini-PC collectors. A session does not require the
VPS, Telegram, or internet access. Raw NCSI datagrams are written before DSP or
terminal rendering and remain the authoritative evidence.

## Architecture

`retrack.collector.ReTrackCore` owns dynamic node, topology, geometry, sync,
DSP, and recording state. `NetworkRuntime` owns only the local UDP sockets,
NCOL announcement, lease renewal, and ingestion loop. The terminal UI reads
snapshots from that headless core. `retrack replay` feeds archived envelopes
back into the same `ReTrackCore.ingest()` and therefore uses the same Phase-5
DSP and Phase-6 synchronization implementation as live collection.

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

## Install and run

Termux:

```sh
pkg install python git
cd /path/to/newo/experiments/wifi-sensing-v1/tools
python -m pip install -e .
retrack --room bedroom run
```

Normal Linux uses the same final three commands in a Python 3.10+ virtual
environment. Copy `retrack.example.json` to a private configuration path and
pass `--config PATH` for named rooms and persistent storage choices. Defaults
write under `~/retrack-data`, use `~/.config/retrack/nodes.json`, and keep all
VPS publishing disabled.

Other commands:

```sh
retrack --room bedroom catalog
retrack --room bedroom replay ~/retrack-data/sessions/SESSION_ID
retrack --room bedroom daemon --track-on --record --label baseline
```

## Phase 7A limitations

- The initial `retrackd` boundary is headless but has no IPC client yet; a TUI
  launched directly still shares its process with the runtime.
- `C` reserves the calibration action and the geometry model supports
  `REPOSITIONING -> SETTLING -> CALIBRATION_REQUIRED -> BUILDING -> READY`, but
  Phase 7B calibration/model work is not started here.
- Node discovery is the Newo leader control response plus identities learned
  from received NCSI records. Full onboarding, credentials, firmware flashing,
  four-node RF scheduling, and authenticated pairing are deferred.
- `export` is an explicit namespace only. No automatic WAN upload occurs while
  tracking or recording.
- RF states remain conservative research evidence. ReTrack does not claim
  identity, localization, pose, occupancy, or a detected person.
