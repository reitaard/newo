# Newo CSI experiment probe protocol v1

The Phase 3 probe is a fixed 28-byte little-endian ESP-NOW application payload.
It is controlled traffic for the `NEWO2_NEWO` RF path, not a CSI record and not
a synchronization record.

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `NPRB` |
| 4 | 1 | protocol version | `1` |
| 5 | 1 | reserved | zero |
| 6 | 2 | header length | `28` |
| 8 | 4 | sender node ID | configured Newo2 node identity |
| 12 | 4 | probe sequence | wraps modulo 2^32 |
| 16 | 8 | sender timestamp | local monotonic microseconds |
| 24 | 4 | CRC-32C | Castagnoli CRC over all 28 bytes with this field zero |

The receiver accepts probes only from the configured Newo2 station MAC and
validates magic, version, length, and CRC. The timestamp is sender-local and
must not be treated as synchronized wall time. Probe sequence gaps diagnose
application/link loss; CSI sequence gaps diagnose the separate capture queue
and UDP path.
