"""Streaming capture statistics without retaining CSI payloads in memory."""

from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Any

from .protocol import CsiRecord, DiagnosticRecord, PATH_NAMES, Record, StatusRecord, mac_text


@dataclass
class _PathStats:
    count: int = 0
    first_ns: int | None = None
    last_ns: int | None = None
    rssi_sum: int = 0
    rssi_min: int = 127
    rssi_max: int = -128
    lengths: Counter[int] = field(default_factory=Counter)
    sources: set[str] = field(default_factory=set)
    receivers: set[str] = field(default_factory=set)
    channels: set[int] = field(default_factory=set)


class CaptureStats:
    def __init__(self) -> None:
        self.total_records = 0
        self.csi_frames = 0
        self.status_records = 0
        self.sync_records = 0
        self.diagnostic_records = 0
        self.unknown_records = 0
        self.rejected_datagrams = 0
        self.rejection_reasons: Counter[str] = Counter()
        self.first_host_ns: int | None = None
        self.last_host_ns: int | None = None
        self.paths: dict[int, _PathStats] = defaultdict(_PathStats)
        self._last_sequence: dict[tuple[int, bytes], int] = {}
        self.sequence_gaps: Counter[str] = Counter()
        self.out_of_order: Counter[str] = Counter()
        self.duplicates: Counter[str] = Counter()
        self.latest_status: dict[tuple[int, int], StatusRecord] = {}
        self.first_status: dict[tuple[int, int], StatusRecord] = {}
        self._boot_by_receiver: dict[tuple[int, bytes], int] = {}
        self.latest_diagnostic: dict[tuple[int, int], DiagnosticRecord] = {}
        self.diagnostic_sequence_gaps: Counter[str] = Counter()
        self._last_diagnostic_sequence: dict[tuple[int, int], int] = {}

    def reject(self, reason: str = "protocol") -> None:
        self.rejected_datagrams += 1
        self.rejection_reasons[reason] += 1

    def add(self, record: Record, host_ns: int) -> None:
        self.total_records += 1
        self.first_host_ns = host_ns if self.first_host_ns is None else min(self.first_host_ns, host_ns)
        self.last_host_ns = host_ns if self.last_host_ns is None else max(self.last_host_ns, host_ns)
        if isinstance(record, CsiRecord):
            self._add_csi(record, host_ns)
        elif isinstance(record, StatusRecord):
            self.status_records += 1
            status_key = (record.node_id, record.boot_id)
            self.first_status.setdefault(status_key, record)
            self.latest_status[status_key] = record
            receiver_key = (record.node_id, record.receiver_mac)
            previous_boot = self._boot_by_receiver.get(receiver_key)
            if previous_boot is not None and previous_boot != record.boot_id:
                self._last_sequence.pop(receiver_key, None)
            self._boot_by_receiver[receiver_key] = record.boot_id
        elif record.record_type == 3:
            self.sync_records += 1
        elif isinstance(record, DiagnosticRecord):
            self.diagnostic_records += 1
            key = (record.node_id, record.boot_id)
            previous = self._last_diagnostic_sequence.get(key)
            if previous is not None:
                delta = (record.diagnostic_sequence - previous) & 0xFFFFFFFF
                if 0 < delta < 0x80000000:
                    self.diagnostic_sequence_gaps[f"node={record.node_id},boot={record.boot_id}"] += delta - 1
            self._last_diagnostic_sequence[key] = record.diagnostic_sequence
            self.latest_diagnostic[key] = record
        else:
            self.unknown_records += 1

    def _add_csi(self, record: CsiRecord, host_ns: int) -> None:
        self.csi_frames += 1
        path = self.paths[record.path_id]
        path.count += 1
        path.first_ns = host_ns if path.first_ns is None else min(path.first_ns, host_ns)
        path.last_ns = host_ns if path.last_ns is None else max(path.last_ns, host_ns)
        path.rssi_sum += record.rssi_dbm
        path.rssi_min = min(path.rssi_min, record.rssi_dbm)
        path.rssi_max = max(path.rssi_max, record.rssi_dbm)
        path.lengths[record.csi_payload_length] += 1
        path.sources.add(mac_text(record.source_mac))
        path.receivers.add(mac_text(record.receiver_mac))
        path.channels.add(record.channel)

        key = (record.node_id, record.receiver_mac)
        label = f"node={record.node_id},receiver={mac_text(record.receiver_mac)}"
        previous = self._last_sequence.get(key)
        if record.sequence == 0 and record.csi_flags & (1 << 7):
            previous = None
        if previous is not None:
            delta = (record.sequence - previous) & 0xFFFFFFFF
            if delta == 0:
                self.duplicates[label] += 1
            elif delta < 0x80000000:
                self.sequence_gaps[label] += delta - 1
            else:
                self.out_of_order[label] += 1
        if previous is None or ((record.sequence - previous) & 0xFFFFFFFF) < 0x80000000:
            self._last_sequence[key] = record.sequence

    def as_dict(self) -> dict[str, Any]:
        duration_s = 0.0
        if self.first_host_ns is not None and self.last_host_ns is not None:
            duration_s = max(0.0, (self.last_host_ns - self.first_host_ns) / 1e9)
        paths: dict[str, Any] = {}
        for path_id, value in sorted(self.paths.items()):
            span = 0.0 if value.first_ns is None or value.last_ns is None else (
                value.last_ns - value.first_ns) / 1e9
            rate = 0.0 if span <= 0 else (value.count - 1) / span
            paths[PATH_NAMES.get(path_id, f"PATH_{path_id}")] = {
                "path_id": path_id,
                "frame_count": value.count,
                "packet_rate_hz": round(rate, 3),
                "rssi_dbm": {
                    "min": value.rssi_min,
                    "mean": round(value.rssi_sum / value.count, 3),
                    "max": value.rssi_max,
                },
                "csi_length_distribution": {str(k): v for k, v in sorted(value.lengths.items())},
                "source_macs": sorted(value.sources),
                "receiver_macs": sorted(value.receivers),
                "channels": sorted(value.channels),
            }
        status = []
        for (node, boot), value in sorted(self.latest_status.items()):
            first = self.first_status[(node, boot)]
            delta = lambda end, start: (end - start) & 0xFFFFFFFF
            status.append({
                "node_id": node, "boot_id": boot,
                "callbacks_total": value.callbacks_total,
                "intentional_rate_gate_drops": value.rate_gate_drops,
                "source_filter_drops": value.source_filter_drops,
                "accepted_total": value.accepted_total,
                "device_ring_drops": value.ring_full_drops,
                "device_transport_ok": value.transport_ok,
                "device_transport_drops": value.transport_drops,
                "session_delta_estimate": {
                    "callbacks": delta(value.callbacks_total, first.callbacks_total),
                    "intentional_rate_gate_drops": delta(value.rate_gate_drops, first.rate_gate_drops),
                    "source_filter_drops": delta(value.source_filter_drops, first.source_filter_drops),
                    "accepted": delta(value.accepted_total, first.accepted_total),
                    "device_ring_drops": delta(value.ring_full_drops, first.ring_full_drops),
                    "device_transport_ok": delta(value.transport_ok, first.transport_ok),
                    "device_transport_drops": delta(value.transport_drops, first.transport_drops),
                },
            })
        diagnostics = []
        for (node, boot), value in sorted(self.latest_diagnostic.items()):
            diagnostics.append({
                "node_id": node, "boot_id": boot,
                "association_epoch": value.association_epoch,
                "status_transport_ok": value.status_transport_ok,
                "status_transport_drops": value.status_transport_drops,
                "probe_tx_attempted": value.probe_tx_attempted,
                "probe_tx_queued": value.probe_tx_queued,
                "probe_tx_success": value.probe_tx_success,
                "probe_tx_link_failure": value.probe_tx_link_failure,
                "probe_tx_submit_failure": value.probe_tx_submit_failure,
                "probe_tx_skipped_busy": value.probe_tx_skipped_busy,
                "probe_tx_skipped_unassociated": value.probe_tx_skipped_unassociated,
                "probe_rx_valid": value.probe_rx_valid,
                "probe_rx_invalid": value.probe_rx_invalid,
                "path_gate_drops": {
                    "ROUTER_NEWO": value.path_1_gate_drops,
                    "ROUTER_NEWO2": value.path_2_gate_drops,
                    "NEWO2_NEWO": value.path_3_gate_drops,
                },
            })
        return {
            "duration_seconds": round(duration_s, 6),
            "record_counts": {"total": self.total_records, "csi": self.csi_frames,
                              "status": self.status_records, "sync": self.sync_records,
                              "diagnostic": self.diagnostic_records,
                              "unknown": self.unknown_records,
                              "rejected_datagrams": self.rejected_datagrams},
            "rejection_reasons": dict(sorted(self.rejection_reasons.items())),
            "sequence_gap_estimate_by_receiver": dict(sorted(self.sequence_gaps.items())),
            "duplicates_by_receiver": dict(sorted(self.duplicates.items())),
            "out_of_order_by_receiver": dict(sorted(self.out_of_order.items())),
            "diagnostic_sequence_gap_estimate": dict(sorted(self.diagnostic_sequence_gaps.items())),
            "paths": paths,
            "latest_device_drop_counters": status,
            "latest_device_diagnostics": diagnostics,
        }
