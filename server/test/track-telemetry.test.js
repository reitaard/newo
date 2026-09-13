import assert from "node:assert/strict";
import test from "node:test";

import { createTrackTelemetryCache, renderTrackPanel, sanitizeTrackTelemetry } from "../src/track-telemetry.js";

function sample(overrides = {}) {
  return {
    schema: "newo_track_telemetry_v1", generated_at_unix_ns: 1_789_000_000_000_000_000,
    room: "bedroom", placement: "EAST", geometry_state: "READY",
    collector: { kind: "PHONE", state: "ANNOUNCING:5005", recording: true, records: 20, rejected: 0 },
    nodes: { newo: { online: true }, newo2: { online: true } },
    paths: [1, 2, 3].map((path_id) => ({ path_id, name: `P${path_id}`, available: true,
      hz: 20 + path_id, rssi_dbm: -50 - path_id, quality: "GOOD", score: .2 * path_id,
      state: path_id === 3 ? "RF_CHANGE" : "QUIET", samples: 100, sequence_gaps: path_id - 1, duplicates: 0 })),
    path_summary: { available: 3, expected: 3, loss_percent: .42 }, receiver_loss: [],
    fusion: { state: "RF_CHANGE", confidence: .81 }, calibration: { status: "MATCH", placement: "EAST" },
    sync: { state: "SYNC_VALID", sample_count: 440, accepted_samples: 438, rejected_samples: 2,
      final_offset_us: 1842, drift_ppm: 6.2, last_sync_age_us: 300000, jitter_us: 286 }, ...overrides,
  };
}

test("versioned telemetry is bounded and rejects malformed paths", () => {
  assert.equal(sanitizeTrackTelemetry(sample()).paths.length, 3);
  assert.throws(() => sanitizeTrackTelemetry({ ...sample(), schema: "future" }));
  assert.throws(() => sanitizeTrackTelemetry({ ...sample(), paths: [{ path_id: 1 }, { path_id: 1 }] }));
});

test("cache reports missing, fresh, and stale collector telemetry", () => {
  let now = 1000;
  const cache = createTrackTelemetryCache({ staleMs: 6000, now: () => now });
  assert.equal(cache.snapshot().stale, true);
  cache.update(sample()); assert.equal(cache.snapshot().stale, false);
  now += 6001; assert.equal(cache.snapshot().stale, true);
});

test("panel renders valid sync, three paths, and evidence without person claims", () => {
  const body = renderTrackPanel({ firmware: { desired: true, actual: "active", connected: true },
    telemetry: { value: sanitizeTrackTelemetry(sample()), stale: false, ageMs: 10 }, now: new Date(0) });
  assert.match(body, /Sync  VALID/); assert.match(body, /438\/2 accepted\/rejected/);
  assert.match(body, /P1.*21\.0Hz/); assert.match(body, /P3.*RF_CHANGE/);
  assert.match(body, /Paths   3\/3/); assert.match(body, /Geometry READY/);
  assert.doesNotMatch(body, /person detected|localization/i);
});

test("panel represents partial paths, stale sync, missing Newo2 and invalid calibration", () => {
  const value = sample({
    nodes: { newo: { online: true }, newo2: { online: false } },
    paths: [{ path_id: 1, name: "P1", available: true, hz: 12, rssi_dbm: -60, quality: "LOW", state: "LOW_CONFIDENCE" },
      { path_id: 2, name: "P2", available: false }, { path_id: 3, name: "P3", available: false }],
    path_summary: { available: 1, expected: 3, loss_percent: null },
    sync: { state: "SYNC_STALE", accepted_samples: 8, rejected_samples: 4 },
    calibration: { status: "REJECTED", reason: "geometry mismatch" }, geometry_state: "REPOSITIONING",
  });
  const body = renderTrackPanel({ firmware: { desired: true, actual: "active" },
    telemetry: { value: sanitizeTrackTelemetry(value), stale: true, ageMs: 9000 } });
  assert.match(body, /Sync  STALE/); assert.match(body, /P2  --Hz/); assert.match(body, /Newo2 OFF/);
  assert.match(body, /Geometry REPOSITIONING/); assert.match(body, /Cal     REJECTED/); assert.match(body, /STALE 9\.0s/);
});

test("debug includes sessions and receiver loss while compact panel does not", () => {
  const value = sample({ receiver_loss: [{ node_id: 2, receiver_mac: "aa", gaps: 7, duplicates: 1 }],
    sync: { state: "SYNC_DEGRADED", leader_session_id: 11, follower_session_id: 22, sample_count: 9 } });
  const telemetry = { value: sanitizeTrackTelemetry(value), stale: false, ageMs: 0 };
  assert.doesNotMatch(renderTrackPanel({ firmware: {}, telemetry }), /DEBUG/);
  assert.match(renderTrackPanel({ firmware: {}, telemetry, debug: true }), /Leader 11  follower 22/);
  assert.match(renderTrackPanel({ firmware: {}, telemetry, debug: true }), /RX N2 gaps 7 dup 1/);
});
