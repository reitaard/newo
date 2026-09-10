# Static CSI transport-drop audit

Scope: source inspection only. No firmware change, device contact, or live-room
measurement was performed.

## What `device_transport_drops` means

The CSI callback assigns a sequence number before checking ring capacity. If
the ring is full it increments `ring_full_drops` and returns. Successfully
enqueued slots are later serialized by `sender_task`. Only there, after removal
from the ring head, does a zero serialization length or a `sendto()` result not
equal to the datagram length increment `transport_drops`. The ring head is then
advanced regardless of success.

Therefore:

- `ring_full_drops` happens at the callback-to-sender boundary, before the ring;
- `transport_drops` happens after a frame entered the ring;
- a CSI sequence gap can reflect either class because numbering precedes the
  ring-capacity check and failed transmitted records are also never archived;
- `status_transport_drops` is separate and covers STATUS serialization/send;
- DIAGNOSTIC send failures are logged but do not currently have a counter.

Serialization failure is unlikely for a previously validated fixed-size slot
unless memory/corruption or an unsupported geometry is present. The plausible
normal cause is `sendto()` failure. The current boolean wrapper discards
`errno`, send duration, association state, and the actual return value, so the
existing counters cannot distinguish local lwIP/TX-buffer pressure from route,
interface, or collector-address failures.

ESP-IDF documents that repeated UDP `sendto()` can fail with `ENOMEM` when
lower-layer transmit buffers are full. Bursts are therefore consistent with
temporary Wi-Fi/lwIP backpressure. UDP does not provide receiver delivery
acknowledgement: a successful `sendto()` only establishes local acceptance, so
AP loss, phone/laptop receive-buffer overflow, firewall loss, and RF retries can
produce host sequence gaps without incrementing device transport drops.

Wi-Fi coexistence is plausible only as an indirect load/airtime contributor.
ESP-NOW probes, gateway ping/replies, CSI callbacks, UDP egress, management
traffic, and any other same-radio traffic share the interface. That can exhaust
TX buffers or delay the sender task. It does not by itself identify the failing
layer, and speaker audio/vibration in the room is not evidence of network or
human-motion causation.

## Proposed instrumentation (not applied)

Keep all additions in the sender/transport task, never the CSI callback:

1. Capture `send_start_us`, `send_end_us`, returned byte count, and `errno`
   immediately after every failed `sendto()`.
2. Add cumulative error buckets for at least `ENOMEM`, `ENETDOWN`,
   `ENETUNREACH`, `EHOSTUNREACH`, other errno, short write, and serialization
   failure.
3. Track maximum and histogram buckets for send duration, plus consecutive
   failure burst length and first/last failure monotonic timestamps.
4. Record ring occupancy/high-water mark at each send attempt and failure.
5. Snapshot association epoch, associated flag, free heap/minimum free heap,
   path ID, CSI record length, and sender-task loop delay at failure time.
6. Count DIAGNOSTIC send failures separately so loss of the diagnostic channel
   is visible.
7. Extend a future versioned diagnostic record rather than changing NCSI v1 or
   overloading the existing counters. Preserve old parsers and exact raw CSI.

These fields distinguish:

- high ring occupancy plus slow sends: sender/lwIP backpressure;
- `ENOMEM` with normal association: local TX-buffer exhaustion;
- network-down/unreachable errors near association-epoch change: link/route;
- device sends successful but host sequence gaps: RF/AP/host receive path;
- serialization bucket increments: local record construction defect;
- failures confined to one record size/geometry: size/geometry interaction;
- STATUS and CSI failing together: shared socket/network pressure rather than a
  CSI-only code path.

Retries should not be added casually: delayed retransmission changes cadence,
can reorder experimental records, and may worsen congestion. First collect the
above evidence; if retry is later justified, bound it in the sender task and
continue reporting original timestamp/sequence and retry outcome explicitly.
