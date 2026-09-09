# Newo Wi-Fi CSI sensing experiment v1

Status: Phase 1 protocol, isolated Newo receiver under [`newo-rx/`](./newo-rx/),
and the Phase 3 GOOUUU ESP32-S3-CAM radio node under
[`newo2-node/`](./newo2-node/). Nothing is integrated into production `Newo/`,
the camera remains disabled, and no ESP has been flashed.

## Goal and boundaries

This experiment will collect raw 2.4 GHz Wi-Fi Channel State Information (CSI) from two ESP32-S3 devices in one fixed room:

- **Newo**: ESP32-S3, 16 MB flash, 8 MB PSRAM.
- **Newo2**: GOOUUU ESP32-S3-CAM, 16 MB flash, 8 MB PSRAM, OV3660 camera, microSD.
- **Router**: an ordinary, fixed 2.4 GHz home access point.

The retained measurements should support later, separately validated research into presence, movement, zone, direction, activity, person count, gait/identity, respiration, and pose. Retaining a signal does not establish that any of those outcomes are feasible or accurate.

The experiment deliberately excludes pose models or claims, heart-rate claims, person-count heuristics, fall classification, WASM, Matter/Home Assistant, channel hopping, 5 GHz, ESP32-C6 features, NDP injection, mesh operation, and the wider RuView platform.

## Physical and data architecture

```text
                         fixed 2.4 GHz router
                        /                    \
             ROUTER_NEWO                      ROUTER_NEWO2
                      /                        \
                  Newo  <---- NEWO2_NEWO ---- Newo2 + OV3660
                    |                              |
                    +----- versioned NCSI records-+
                                      |
                              bounded device queues
                                      |
                              host collector/storage
                               /                  \
                    session metadata          raw CSI archive
                  (labels, camera IDs)       (radio facts only)
                                      |
                        offline sanitation / phase / DSP
```

Each receiver joins the router and stays on the connected access point's channel. No channel hopping is allowed in v1. Newo and Newo2 capture CSI in the Wi-Fi receive callback, verify the transmitter against an explicit MAC allowlist, sanitize invalid leading CSI bytes, rate-gate accepted frames, and enqueue fixed-size descriptors into a bounded ring. A non-callback task serializes and transports records to the host.

The callback must not allocate, block, perform DSP, write to storage, or send network packets. Ring-full and rate-gate losses are counted rather than hidden. Raw transport and DSP have separate clocks: retained raw records target **20 Hz per active path** initially and are configurable up to **50 Hz per path**; later DSP may consume a lower uniform cadence without changing or suppressing the raw archive. A faster callback arrival rate is expected and must be gated. This design never assumes 100 Hz is required.

## Initial measurement paths

| Path ID | Receiver | Required source identity | Intended traffic |
| --- | --- | --- | --- |
| `ROUTER_NEWO` (`1`) | Newo | Router BSSID/source MAC | Router traffic, including controlled gateway replies |
| `ROUTER_NEWO2` (`2`) | Newo2 | Router BSSID/source MAC | Router traffic, including controlled gateway replies |
| `NEWO2_NEWO` (`3`) | Newo | Newo2 station MAC | Deliberate Newo2-to-Newo packets |

Path assignment is a host/configuration lookup over the tuple `(receiver_mac, source_mac)`. Router and Newo2 identity must come from observed/configured MAC addresses, never packet timing, RSSI, node proximity, or other inference. A frame with an unknown or disallowed source is not silently assigned a path.

Gateway self-ping is the initial controlled OFDM traffic source for the two router paths: after association, a node discovers the gateway address and sends small ICMP echo requests at a configured cadence. The resulting router replies provide receive-side traffic. This does not prove that every accepted router frame is a ping reply, so the CSI record describes the observed source MAC and the session/control log describes when self-ping was enabled.

`NEWO2_NEWO` needs an explicit, separately scheduled Newo2 transmitter in a firmware phase. It must use Newo2's configured station MAC and must not be inferred from ambient packets. `NEWO_NEWO2` is reserved for a later protocol revision/extension and is not an initial path.

## Capture contract

The future firmware implementation must:

1. Join the fixed 2.4 GHz router, query the associated AP channel, and remain on it.
2. Configure ESP32-S3 CSI collection for the chosen legacy/HT LTFs and preserve the receive metadata needed to interpret buffer geometry.
3. Copy the receiver identity and compare `wifi_csi_info_t.mac` with an explicit source-MAC allowlist before path assignment.
4. If `first_word_invalid` is set, zero up to the first four invalid bytes in place, record the sanitized-byte count, and set both corresponding flags. Preserve the original payload length and I/Q positions; never feed the invalid values into DSP.
5. Assign a node-local sequence number to every accepted post-filter, post-rate-gate CSI record. Sequence gaps therefore expose downstream queue/transport loss; callback and gate counters expose earlier loss.
6. Timestamp accepted records from the local monotonic microsecond clock. Wall-clock labels belong to the host metadata, not the CSI callback.
7. Copy raw signed 8-bit complex samples in ESP-IDF order: imaginary byte, then real byte. Do not convert to magnitude or phase in the raw record.
8. Apply the cadence gate independently after source/path filtering for each active path, so unrelated traffic and one busy source cannot consume another path's budget. Gate retained raw records to 20 Hz per path by default, configurable from 1 through 50 Hz. Do not burst to catch up after delayed callbacks.
9. Push into a fixed-capacity single-producer/single-consumer ring and increment `ring_full_drops` if no slot is available.
10. Emit periodic `STATUS` records with packet-yield and loss counters. Keep raw-record cadence independent of later DSP cadence.

See [PROTOCOL.md](./PROTOCOL.md) for the exact byte contract and host session schema.

## Future DSP baseline

Offline processing should begin from the archived I/Q payload, grouped by session and path. Initial, testable building blocks are:

- derive amplitude and phase with `atan2(imag, real)`;
- unwrap phase over time independently for each stable subcarrier/LTF geometry;
- maintain numerically stable running mean and variance (for example, Welford statistics);
- select top-K subcarriers by an explicitly documented quality/variance score;
- reset or partition state when channel, bandwidth, PHY/LTF geometry, receiver, source, or session changes;
- record actual input cadence and missing sequences instead of assuming uniform 100 Hz data.

Top-K selection is a feature-selection primitive, not evidence for person count, health measurements, identity, or pose.

## Phase roadmap

1. **Phase 1 — protocol and experiment design (this directory):** freeze record v1, paths, metadata separation, diagnostics, scope, and attribution.
2. **Phase 2 — isolated receiver firmware:** create experiment-only ESP32-S3 CSI capture targets for Newo and Newo2; add host-side serialization tests; do not merge into production `Newo/` firmware.
3. **Phase 3 — host collector and calibration:** validate CRC/length/sequence handling, store session metadata separately, measure achieved yield and drops, and collect empty-room baselines for the two router paths.
4. **Phase 4 — third path and synchronization:** add deliberate Newo2 traffic for `NEWO2_NEWO`, emit synchronization records, and quantify cross-node clock offset/drift before combining paths.
5. **Phase 5 — offline DSP:** implement geometry-aware phase extraction/unwrapping, running statistics, top-K selection, and reproducible signal-quality reports.
6. **Phase 6 — labeled feasibility studies:** evaluate one target at a time with held-out sessions and camera-derived labels where consented. Report negative results and uncertainty; make no health or safety claims.

## Data and safety notes

- CSI and camera-derived labels can reveal occupancy, behavior, or identity. Obtain consent, minimize retention, restrict access, and define deletion rules before captures involving people.
- Wi-Fi credentials and secrets belong in untracked runtime provisioning, never source control or session metadata.
- Camera media is not embedded in radio records. `camera_frame_id` is an optional host-side join key only.
- Generated captures, binaries, build directories, and secret-bearing `sdkconfig` files must remain untracked.
- No hardware was flashed or exercised in Phase 1.

## Phase 2 build and configuration

The standalone receiver is documented in [`newo-rx/README.md`](./newo-rx/README.md). It is tested against ESP-IDF v5.5.5 and can be built reproducibly with the official `espressif/idf:v5.5.5` Docker image. Configure Wi-Fi credentials and the collector through `idf.py menuconfig`; the generated `sdkconfig` and `build/` directory are ignored and must not be committed.

The normal workflow from `experiments/wifi-sensing-v1/newo-rx/` is:

```sh
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

The Docker commands, host protocol test, configuration fields, and future run/monitor expectations are in the receiver README. Phase 2 builds only; it does not authorize flashing.

## Phase 3 ESP-NOW path

Phase 3 adds one-way, versioned Newo2-to-Newo ESP-NOW probes while both ESPs
remain ordinary stations on the same AP. The peer is bound to `WIFI_IF_STA` with
channel `0` so it follows the associated interface channel; there is no channel
hopping. See [`newo2-node/README.md`](./newo2-node/README.md) for topology and
explicit MAC configuration, and [`PROBE_PROTOCOL.md`](./PROBE_PROTOCOL.md) for
the 28-byte probe header. Bidirectional probes, camera streaming, ML, and NDP
injection remain out of scope.

## Source lineage

The measurement-plane design adapts a narrow set of implementation ideas reviewed in RuView, including CSI callback configuration, source-MAC filtering, AP channel detection, gateway self-ping, early rate limiting, fixed rings and loss counters, separate raw/DSP cadences, invalid-first-word sanitation, sequence/metadata capture, phase unwrapping, running statistics, and top-K selection. RuView is MIT licensed; see [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md). The Newo wire format is new and is not RuView's wire format.
