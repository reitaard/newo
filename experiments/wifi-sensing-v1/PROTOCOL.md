# Newo CSI protocol v1

This document specifies the experiment's device-to-host record stream and separate host-managed session metadata. Protocol integers are unsigned unless marked otherwise, all multibyte integers are little-endian, and byte offsets are zero-based.

## Framing and evolution

Every record starts with the same 16-byte header. Records may be carried one-per-UDP-datagram or concatenated in a reliable byte stream; a parser uses `record_length` to advance. v1 has no compression or encryption. Network isolation or a future authenticated envelope is required before use on an untrusted network.

| Offset | Size | Field | v1 value / meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | ASCII `NCSI` (`4e 43 53 49`) |
| 4 | 1 | `version_major` | `1` |
| 5 | 1 | `record_type` | `1=CSI`, `2=STATUS`, `3=SYNC` |
| 6 | 2 | `header_length` | Bytes through the type-specific header |
| 8 | 4 | `record_length` | Header plus payload; maximum `4096` in v1 |
| 12 | 4 | `crc32c` | CRC-32C (Castagnoli, reflected polynomial `0x82F63B78`, init/final XOR `0xFFFFFFFF`) over the complete record with these four bytes zeroed |

A receiver must reject a bad magic, unsupported major version, length below `header_length`, length above 4096, type-specific length mismatch, odd CSI payload length, or CRC failure. Unknown record types with a valid common header can be skipped by `record_length`. New minor-compatible fields require a larger `header_length`; v1 parsers ignore header bytes beyond those they know. A breaking reinterpretation requires a new `version_major`.

## CSI record (`record_type = 1`)

The CSI header is exactly 64 bytes including the common header. Its payload begins at offset 64 and contains `csi_payload_length` signed 8-bit samples.

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 16 | 4 | `node_id` | Provisioned stable numeric receiver node ID; `1=Newo`, `2=Newo2` initially |
| 20 | 6 | `receiver_mac` | Station MAC of the ESP that captured the frame |
| 26 | 6 | `source_mac` | Transmitter MAC reported by the CSI callback; exact identity, not inferred |
| 32 | 4 | `sequence` | Node-local accepted-CSI counter, incremented once per emitted CSI record; wrap allowed |
| 36 | 8 | `timestamp_us` | Receiver-local monotonic microseconds; no wall-clock meaning |
| 44 | 1 | `channel` | Observed 2.4 GHz primary Wi-Fi channel |
| 45 | 1 | `secondary_channel` | `0=none`, `1=above`, `2=below`, `255=unknown` |
| 46 | 1 | `bandwidth` | `0=20 MHz`, `1=40 MHz`, `255=unknown` |
| 47 | 1 | `phy_mode` | `0=legacy/non-HT`, `1=HT`, `255=unknown` |
| 48 | 1 | `rssi_dbm` | Signed receive RSSI in dBm; `-128=unavailable` |
| 49 | 1 | `noise_floor_dbm` | Signed RF noise floor in dBm; `-128=unavailable` |
| 50 | 1 | `antenna` | ESP receive antenna index; `255=unknown` |
| 51 | 1 | `ltf_mask` | Bit 0 `LLTF`, bit 1 `HT_LTF`, bit 2 `STBC_HT_LTF`; zero if unknown |
| 52 | 2 | `driver_csi_length` | Original CSI buffer byte length reported by ESP-IDF before sanitation |
| 54 | 2 | `csi_payload_length` | Bytes retained in this record; equals `record_length - header_length` |
| 56 | 2 | `subcarrier_item_count` | Number of complex byte pairs retained; `csi_payload_length / 2` |
| 58 | 2 | `csi_flags` | Bit field below |
| 60 | 2 | `path_id` | `1=ROUTER_NEWO`, `2=ROUTER_NEWO2`, `3=NEWO2_NEWO`, `0=UNKNOWN` |
| 62 | 1 | `discarded_prefix_bytes` | `0` normally; up to `4` when invalid leading bytes were removed |
| 63 | 1 | `iq_order` | `1=IMAG_REAL_S8`; all other values unsupported in v1 |

### CSI flags

| Bit | Name | Meaning when set |
| ---: | --- | --- |
| 0 | `FIRST_WORD_INVALID_REPORTED` | ESP-IDF reported `first_word_invalid` |
| 1 | `INVALID_PREFIX_REMOVED` | `discarded_prefix_bytes` were omitted from the payload |
| 2 | `SOURCE_FILTER_MATCHED` | Source matched an explicit configured allowlist entry |
| 3 | `RX_METADATA_VALID` | Driver indicated receive/channel metadata is valid where that indication is available |
| 4 | `PAYLOAD_TRUNCATED` | Buffer exceeded the record capacity; such records should normally be dropped instead |
| 5 | `STBC` | Receive metadata identifies an STBC packet |
| 6 | `CONTROL_TRAFFIC_ACTIVE` | A controlled traffic generator was enabled at capture time; it does not assert packet causality |
| 7 | `SEQUENCE_RESET` | First record after boot/counter reset |
| 8-15 | — | Reserved; writers set zero, readers ignore |

For ESP32-S3 v1, each complex item is two signed bytes in ESP-IDF order `(imaginary, real)`. If `first_word_invalid` is true, the writer sets bits 0 and 1, sets `discarded_prefix_bytes = min(4, driver_csi_length)`, and starts the payload after that prefix. Therefore:

```text
csi_payload_length = driver_csi_length - discarded_prefix_bytes
subcarrier_item_count = floor(csi_payload_length / 2)
```

If sanitation leaves an odd number of bytes, the record is rejected rather than guessing an I/Q pairing. The host must group data by receiver, source, channel, bandwidth, PHY mode, LTF mask, and length geometry before interpreting subcarrier indices. `subcarrier_item_count` counts retained complex items across included LTF blocks; it is not necessarily the count for one LTF.

## Status record (`record_type = 2`)

`STATUS` is a separate 80-byte, payload-free diagnostic record. Counters are cumulative since `boot_id` and wrap modulo 2^32. Comparing consecutive records yields interval diagnostics.

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 16 | 4 | `node_id` | Receiver node ID |
| 20 | 6 | `receiver_mac` | Receiver station MAC |
| 26 | 2 | `status_flags` | Bit 0 associated, bit 1 CSI enabled, bit 2 source filter enabled, bit 3 self-ping enabled, bit 4 ring healthy |
| 28 | 4 | `boot_id` | Random value generated at boot; distinguishes counter epochs |
| 32 | 4 | `status_sequence` | Node-local status-record counter |
| 36 | 8 | `timestamp_us` | Receiver-local monotonic microseconds |
| 44 | 4 | `callbacks_total` | All CSI callback invocations before gating/filtering |
| 48 | 4 | `rate_gate_drops` | Source-matched callbacks deliberately skipped by the per-path raw cadence gate |
| 52 | 4 | `source_filter_drops` | Frames rejected by source MAC filtering before cadence gating |
| 56 | 4 | `accepted_total` | CSI records accepted after gate/filter checks |
| 60 | 4 | `ring_full_drops` | Accepted records lost because the fixed ring was full |
| 64 | 4 | `transport_ok` | CSI records successfully handed to transport |
| 68 | 4 | `transport_drops` | Serialization/send failures after dequeue |
| 72 | 4 | `last_csi_sequence` | Most recently assigned CSI sequence |
| 76 | 2 | `raw_target_hz` | Configured raw retention target per active path, initially 20 and never above 50 |
| 78 | 2 | `dsp_target_hz` | Independent DSP target; zero when DSP disabled |

The host reports packet yield at minimum as deltas for callbacks, gate drops, filter drops, accepted frames, ring drops, transport successes, and transport drops. It must not interpret intentional rate gating as transport loss.

## Synchronization record (`record_type = 3`)

`SYNC` is separate from CSI so clock observations do not inflate every radio record. It has a 64-byte header and no payload.

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 16 | 4 | `node_id` | Receiver node ID |
| 20 | 6 | `receiver_mac` | Receiver station MAC |
| 26 | 2 | `sync_flags` | Bit 0 host time valid, bit 1 round trip measured; remaining bits reserved |
| 28 | 4 | `boot_id` | Same boot epoch used by `STATUS` |
| 32 | 4 | `sync_sequence` | Node-local sync-record counter |
| 36 | 8 | `local_timestamp_us` | Local monotonic time at the defined sync event |
| 44 | 8 | `host_timestamp_us` | Host Unix epoch microseconds, or zero if unavailable |
| 52 | 4 | `round_trip_us` | Measured request/reply round trip, or zero if unavailable |
| 56 | 4 | `last_csi_sequence` | CSI high-water mark at the sync event |
| 60 | 4 | `sync_source` | `0=unspecified`, `1=host request/reply`; other values reserved |

Phase 1 defines this record but does not choose or implement a synchronization exchange. Future synchronization must document precisely which send/receive event each timestamp represents and estimate offset and drift; copying wall time into CSI frames is not synchronization.

## Host session metadata

Large or human-readable experiment labels are stored once by the host, outside radio records. The canonical v1 representation is UTF-8 JSON, one object per session metadata revision. A data store may normalize it, but exports must preserve these names:

```json
{
  "schema_version": 1,
  "session_id": "2026-09-09-empty-room-001",
  "room_id": "room-a",
  "scenario_label": "empty-room-baseline",
  "person_label": null,
  "zone_label": null,
  "activity_label": null,
  "camera_frame_id": null,
  "notes": "Router and nodes fixed in marked positions"
}
```

| Field | Requirement |
| --- | --- |
| `session_id` | Required, unique, immutable identifier used to join records and metadata |
| `room_id` | Required pseudonymous room identifier; avoid addresses |
| `scenario_label` | Required controlled label such as `empty-room-baseline` |
| `person_label` | Optional pseudonymous label; never place a person's name in radio frames |
| `zone_label` | Optional experiment-defined zone |
| `activity_label` | Optional ground-truth annotation, not a model prediction |
| `camera_frame_id` | Optional opaque ID or time-indexed mapping to consented camera data |
| `notes` | Optional free text; must not contain credentials or secrets |

The collector associates every stored radio record with `session_id` in its storage envelope/index. It does not rewrite the NCSI record or repeat session strings in every device frame. Time-varying labels and camera frame IDs should be stored as host-side annotation events keyed by `session_id` and host time/record range, rather than mutating the radio protocol.

## Example parser checks

For a CSI record whose common header says `header_length=64` and `record_length=444`, a parser expects `csi_payload_length=380` and `subcarrier_item_count=190`. If `driver_csi_length=384`, `FIRST_WORD_INVALID_REPORTED` and `INVALID_PREFIX_REMOVED` must both be set and `discarded_prefix_bytes` must be 4. A mismatch invalidates the record.

Before accepting a path, the host also verifies the configured tuple. For example, `ROUTER_NEWO` is valid only when `receiver_mac` equals Newo's station MAC and `source_mac` equals the configured router BSSID. `path_id` is a convenience field, never stronger evidence than those two addresses.
