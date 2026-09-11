const finite = (value) => Number.isFinite(value) ? value : null;
const text = (value, max = 160) => typeof value === "string" ? value.slice(0, max) : null;
const esc = (value) => String(value ?? "--").replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;");

export const TRACK_TELEMETRY_SCHEMA = "newo_track_telemetry_v1";

export function sanitizeTrackTelemetry(body) {
  if (!body || body.schema !== TRACK_TELEMETRY_SCHEMA || !Array.isArray(body.paths) || body.paths.length > 3) {
    throw new Error("invalid track telemetry schema");
  }
  const paths = body.paths.map((path) => ({
    path_id: Number.isInteger(path?.path_id) && path.path_id >= 1 && path.path_id <= 3 ? path.path_id : null,
    name: text(path?.name, 24), available: path?.available === true,
    hz: finite(path?.hz), rssi_dbm: finite(path?.rssi_dbm), quality: text(path?.quality, 24),
    score: finite(path?.score), state: text(path?.state, 32), selected_subcarriers: finite(path?.selected_subcarriers),
    samples: finite(path?.samples), sequence: finite(path?.sequence), sequence_gaps: finite(path?.sequence_gaps),
    duplicates: finite(path?.duplicates), geometry: text(path?.geometry, 256),
  }));
  if (paths.some((path) => path.path_id === null) || new Set(paths.map((path) => path.path_id)).size !== paths.length) {
    throw new Error("invalid track telemetry paths");
  }
  return {
    schema: TRACK_TELEMETRY_SCHEMA, generated_at_unix_ns: finite(body.generated_at_unix_ns),
    room: text(body.room, 64), placement: text(body.placement, 64), geometry_state: text(body.geometry_state, 32),
    collector: { kind: text(body.collector?.kind, 24), state: text(body.collector?.state, 64),
      recording: body.collector?.recording === true, session_id: text(body.collector?.session_id, 128),
      recording_elapsed_s: finite(body.collector?.recording_elapsed_s), records: finite(body.collector?.records),
      rejected: finite(body.collector?.rejected) },
    nodes: { newo: { online: body.nodes?.newo?.online === true, age_s: finite(body.nodes?.newo?.age_s) },
      newo2: { online: body.nodes?.newo2?.online === true, age_s: finite(body.nodes?.newo2?.age_s) } },
    paths, path_summary: { available: finite(body.path_summary?.available), expected: finite(body.path_summary?.expected),
      loss_percent: finite(body.path_summary?.loss_percent) },
    receiver_loss: Array.isArray(body.receiver_loss) ? body.receiver_loss.slice(0, 8).map((row) => ({
      node_id: finite(row?.node_id), receiver_mac: text(row?.receiver_mac, 32), gaps: finite(row?.gaps), duplicates: finite(row?.duplicates),
    })) : [],
    fusion: { state: text(body.fusion?.state, 32), confidence: finite(body.fusion?.confidence) },
    calibration: { status: text(body.calibration?.status, 32), reason: text(body.calibration?.reason, 160),
      room: text(body.calibration?.room, 64), placement: text(body.calibration?.placement, 64),
      schema_version: finite(body.calibration?.schema_version) },
    sync: { state: text(body.sync?.state, 32), sample_count: finite(body.sync?.sample_count),
      accepted_samples: finite(body.sync?.accepted_samples), rejected_samples: finite(body.sync?.rejected_samples),
      final_offset_us: finite(body.sync?.final_offset_us), drift_ppm: finite(body.sync?.drift_ppm),
      last_sync_age_us: finite(body.sync?.last_sync_age_us), jitter_us: finite(body.sync?.jitter_us),
      leader_session_id: finite(body.sync?.leader_session_id), follower_session_id: finite(body.sync?.follower_session_id),
      session_epoch_count: finite(body.sync?.session_epoch_count), residual_us: body.sync?.residual_us ? {
        median: finite(body.sync.residual_us.median), p95: finite(body.sync.residual_us.p95),
        p99: finite(body.sync.residual_us.p99),
      } : null },
    device_diagnostics: Object.fromEntries(Object.entries(body.device_diagnostics ?? {}).slice(0, 8).map(([node, row]) => [
      text(node, 16), { boot_id: finite(row?.boot_id), callbacks_total: finite(row?.callbacks_total),
        ring_drops: finite(row?.ring_drops), transport_ok: finite(row?.transport_ok), transport_drops: finite(row?.transport_drops),
        probe_tx_attempted: finite(row?.probe_tx_attempted), probe_tx_success: finite(row?.probe_tx_success),
        probe_tx_link_failure: finite(row?.probe_tx_link_failure), probe_rx_valid: finite(row?.probe_rx_valid),
        probe_rx_invalid: finite(row?.probe_rx_invalid) },
    ])),
  };
}

export function createTrackTelemetryCache({ staleMs = 6_000, now = Date.now } = {}) {
  let value = null, receivedAt = null;
  return {
    update(body) { value = sanitizeTrackTelemetry(body); receivedAt = now(); return value; },
    snapshot() { const ageMs = receivedAt === null ? null : Math.max(0, now() - receivedAt);
      return { value, receivedAt, ageMs, stale: ageMs === null || ageMs > staleMs }; },
  };
}

const n = (value, digits = 1) => Number.isFinite(value) ? Number(value).toFixed(digits) : "--";
const stateName = (value) => String(value ?? "N/A").replace(/^SYNC_/, "");

export function renderTrackPanel({ firmware = {}, telemetry = {}, spinner = "◐", debug = false, now = new Date() } = {}) {
  const desired = firmware.desired ? "ON" : "OFF";
  const actual = String(firmware.actual ?? "unknown").toUpperCase();
  const snap = telemetry.value;
  const age = telemetry.ageMs === null ? "N/A" : `${n(telemetry.ageMs / 1000, 1)}s`;
  const generated = Number.isFinite(snap?.generated_at_unix_ns)
    ? new Date(snap.generated_at_unix_ns / 1e6) : now;
  const lines = [`${spinner} <b>TRACK  ${esc(actual)}</b>  desired ${desired}  ${generated.toISOString().slice(11, 19)}`];
  if (!snap) {
    lines.push("", `Collector  N/A  telemetry missing`, `Newo      ${firmware.connected ? "ONLINE" : "OFFLINE"}`);
    return lines.join("\n");
  }
  const sync = snap.sync ?? {};
  const syncAge = Number.isFinite(sync.last_sync_age_us) ? `${n(sync.last_sync_age_us / 1e6, 2)}s` : "--";
  const drift = Number.isFinite(sync.drift_ppm) ? `${sync.drift_ppm >= 0 ? "+" : ""}${n(sync.drift_ppm, 1)}ppm` : "--";
  const offset = Number.isFinite(sync.final_offset_us) ? `${sync.final_offset_us >= 0 ? "+" : ""}${Math.round(sync.final_offset_us)}µs` : "--";
  const jitter = Number.isFinite(sync.jitter_us) ? `j${Math.round(sync.jitter_us)}µs` : "j--";
  lines.push("", `Sync  ${esc(stateName(sync.state))} age ${syncAge} ${offset} ${jitter} ${drift}`,
    `      ${sync.accepted_samples ?? "--"}/${sync.rejected_samples ?? "--"} accepted/rejected`);
  for (const pathId of [1, 2, 3]) {
    const path = snap.paths.find((item) => item.path_id === pathId);
    if (!path?.available) { lines.push(`P${pathId}  --Hz  --dBm  --  N/A`); continue; }
    lines.push(`P${pathId}  ${n(path.hz, 1)}Hz  ${path.rssi_dbm ?? "--"}dBm  ${n(path.score, 2)}  ${esc(path.state ?? "N/A")}/${esc(path.quality)}`);
  }
  const pathCount = snap.path_summary?.available ?? snap.paths.filter((path) => path.available).length;
  const loss = Number.isFinite(snap.path_summary?.loss_percent) ? `${n(snap.path_summary.loss_percent, 2)}%` : "--";
  lines.push("", `Fusion  ${esc(snap.fusion?.state)}  ${n(snap.fusion?.confidence, 2)}`,
    `Paths   ${pathCount}/3  loss ${loss}`,
    `Geometry ${esc(snap.geometry_state)}`,
    `Cal     ${esc(snap.calibration?.status)}  ${esc(snap.calibration?.placement ?? snap.calibration?.reason)}`,
    `Coll    ${esc(snap.collector?.kind)}  ${snap.collector?.recording ? "REC ON" : "REC OFF"}${telemetry.stale ? `  STALE ${age}` : ""}`,
    `Nodes   Newo ${snap.nodes?.newo?.online ? "ON" : "OFF"}  Newo2 ${snap.nodes?.newo2?.online ? "ON" : "OFF"}`);
  if (debug) {
    lines.push("", `<b>DEBUG</b>`,
      `Leader ${sync.leader_session_id ?? "--"}  follower ${sync.follower_session_id ?? "--"}`,
      `Epochs ${sync.session_epoch_count ?? "--"}  sync records ${sync.sample_count ?? "--"}`,
      `Collector state ${esc(snap.collector?.state)} records ${snap.collector?.records ?? "--"} rejected ${snap.collector?.rejected ?? "--"}`);
    for (const row of snap.receiver_loss ?? []) lines.push(`RX N${row.node_id ?? "?"} gaps ${row.gaps ?? "--"} dup ${row.duplicates ?? "--"}`);
    for (const [node, diag] of Object.entries(snap.device_diagnostics ?? {})) lines.push(
      `N${node} boot ${diag.boot_id ?? "--"} ring ${diag.ring_drops ?? "--"} udp ${diag.transport_ok ?? "--"}/${diag.transport_drops ?? "--"} probe ${diag.probe_tx_success ?? "--"}/${diag.probe_tx_link_failure ?? "--"}`);
  }
  lines.push("", `${spinner} ${telemetry.stale ? "collector telemetry stale" : "2s"}`);
  return lines.join("\n");
}
