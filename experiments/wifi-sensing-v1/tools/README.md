# Newo CSI host tools

Python 3.10+ tooling for capturing, inspecting, replaying, and optionally
exporting Newo CSI protocol v1. The canonical capture is not CSV:

```text
datasets/<session_id>/
  session.json    immutable labels plus capture start/end
  events.jsonl    host capture lifecycle events
  frames.ncsi     append-only binary envelopes containing exact UDP records
  summary.json    final counts, rates, RF geometry, and loss diagnostics
```

Each 28-byte `NCAP` envelope stores host receive time, sender IPv4/port, and
record length followed by the original NCSI datagram without re-encoding it.
The strict decoder validates magic, version, lengths, CSI geometry,
first-word sanitation, I/Q order, and CRC-32C before archival.
The collector requests a 4 MiB UDP receive buffer by default and records the
effective OS value in session metadata; sequence/status counters remain the
authoritative indicators of loss.

Run without installation from this directory:

```sh
python -m newo_csi collect --room-id ROOM_A --scenario EMPTY --duration 60 \
  --router-bssid aa:bb:cc:dd:ee:ff \
  --newo-mac 10:11:12:13:14:15 --newo2-mac 20:21:22:23:24:25
python -m newo_csi inspect datasets/<session_id>
python -m newo_csi replay datasets/<session_id> --host 127.0.0.1 --port 5005
python -m newo_csi export datasets/<session_id> --output inspection-export.csv \
  --subcarriers 8,16,24
python -m unittest discover -s tests -v
```

Use `--speed 1` for recorded replay timing, `--speed 2` for twice real time, or
`--speed 0` for no delays. Replay sends the exact embedded NCSI records as UDP
datagrams. CSV export is derived inspection data only; it emits signed
imaginary/real bytes, amplitude, and wrapped phase for selected subcarriers.
It must not replace `frames.ncsi`.

Sequence loss is calculated across each receiver's node-ID/MAC stream because
device CSI sequence numbers are node-local and may interleave paths. Status
records separately expose intentional rate gating, source filtering, ring-full
drops, and device UDP failures; those categories are not conflated.

For reproducible research captures, supply all three MAC options shown above.
They validate the receiver/source tuple for `ROUTER_NEWO`, `ROUTER_NEWO2`, and
`NEWO2_NEWO`; mismatches are rejected and counted. Omitting all three is useful
for initial diagnostics but trusts the transmitted path ID and records a null
mapping in session metadata.
