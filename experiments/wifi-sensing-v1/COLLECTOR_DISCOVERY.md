# Collector discovery design (not deployed)

Status: host/protocol design only. Neither experiment firmware target consumes
announcements yet. Do not flash this design or treat it as active discovery.

## Decision

Keep NCSI CSI/status/diagnostic records on the existing unicast UDP data plane.
Use a separate, very small collector announcement on the local LAN. A phone or
laptop collector multicasts one 18-byte `NCOL` message every five seconds to
IPv4 group `239.255.77.77`, UDP port `47777`, with multicast TTL 1. Nodes listen
only while associated and either seeking a collector or renewing a short lease.

This is preferred over making mDNS mandatory in the first experiment build:

- it needs only one fixed-size parser and one multicast socket;
- the sender IPv4 address is authoritative, so DHCP changes require no embedded
  hostname or DNS assumption;
- it does not add a web server, TCP connection, JSON parser, or dependency;
- the contract can later be advertised as `_newo-csi._udp.local` through mDNS
  without changing the NCSI data plane or field-monitor data model.

## Announcement v1

All fields are little-endian. The datagram is exactly 18 bytes.

| Offset | Size | Field | Rule |
| --- | ---: | --- | --- |
| 0 | 4 | magic | ASCII `NCOL` |
| 4 | 1 | version | `1` |
| 5 | 1 | flags | `0` |
| 6 | 2 | NCSI UDP port | `1..65535` |
| 8 | 2 | lease seconds | `5..300` |
| 10 | 4 | nonce | collector-selected diagnostic value |
| 14 | 4 | reserved | zero |

The collector address is the IPv4 source of the announcement, never an address
inside the payload. Nodes accept only same-interface, TTL-1 announcements after
Wi-Fi association. Address selection precedence is: explicit temporary runtime
override, newest valid announcement, configured fallback. The existing
configured address remains a recovery path during development.

A future node implementation should require two identical announcements before
switching, renew only on a fresh lease, retain the current collector until its
lease expires, and log each switch in diagnostics. It must not switch from the
CSI callback. Selection belongs in the existing transport/control task and must
atomically update only the destination socket address.

## RF and network impact

At an announcement every five seconds, the application payload is 3.6 bytes/s
per active collector. Ethernet/IP/UDP/802.11 framing dominates, but the airtime
remains negligible compared with three 20 Hz CSI paths and normal AP beacons.
Nodes send no discovery response, avoiding response implosion. Announcements do
not create CSI training traffic and are never used as motion features.

Multicast may be filtered by AP client isolation, power-saving policies, VPNs,
or Android/iOS network behavior. Therefore the fallback address and an explicit
runtime override remain required. mDNS has similar multicast limitations and,
with ESP-IDF 5+, adds a separate component dependency. It remains an optional
Phase-12 interoperability layer, not a Phase-5 dependency.

## Security and failure boundaries

This LAN protocol is discovery, not authentication. Any local client could
announce itself and redirect research records. Before production integration,
pairing or an authenticated control channel must authorize collector changes.
Nodes must rate-limit parsing, reject malformed length/version/lease values,
never persist a discovered address as Wi-Fi configuration, and fall back safely
after lease expiry. Multiple collectors require an explicit user-selected
priority/token design; lowest-IP or last-packet-wins behavior is forbidden.

The fixed binary announcement and shared host state can later be bridged to an
HTTP/WebSocket/PWA front end. A browser would consume host-normalized status and
events; it would not decode a different CSI/DSP protocol.
