"""Strict decoder for Newo CSI protocol version 1."""

from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Union

MAGIC = b"NCSI"
VERSION = 1
CSI = 1
STATUS = 2
SYNC = 3
DIAGNOSTIC = 4
CSI_HEADER_SIZE = 88
STATUS_SIZE = 80
LEGACY_SYNC_SIZE = 64
SYNC_SIZE = 112
DIAGNOSTIC_SIZE = 104
MAX_RECORD_SIZE = 4096
PATH_NAMES = {0: "UNKNOWN", 1: "ROUTER_NEWO", 2: "ROUTER_NEWO2", 3: "NEWO2_NEWO"}


class ProtocolError(ValueError):
    pass


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def mac_text(value: bytes) -> str:
    return ":".join(f"{part:02x}" for part in value)


@dataclass(frozen=True)
class Common:
    record_type: int
    header_length: int
    record_length: int
    crc32c: int
    raw: bytes


@dataclass(frozen=True)
class CsiRecord(Common):
    node_id: int
    receiver_mac: bytes
    source_mac: bytes
    sequence: int
    timestamp_us: int
    channel: int
    secondary_channel: int
    bandwidth: int
    phy_mode: int
    rssi_dbm: int
    noise_floor_dbm: int
    antenna: int
    ltf_mask: int
    driver_csi_length: int
    csi_payload_length: int
    subcarrier_item_count: int
    csi_flags: int
    path_id: int
    sanitized_prefix_bytes: int
    iq_order: int
    driver_rx_timestamp_us: int
    phy_rate: int
    mcs: int
    rx_flags: int
    ampdu_count: int
    rx_state: int
    packet_length: int
    driver_rx_sequence: int
    destination_mac: bytes
    iq_bytes: bytes


@dataclass(frozen=True)
class StatusRecord(Common):
    node_id: int
    receiver_mac: bytes
    status_flags: int
    boot_id: int
    status_sequence: int
    timestamp_us: int
    callbacks_total: int
    rate_gate_drops: int
    source_filter_drops: int
    accepted_total: int
    ring_full_drops: int
    transport_ok: int
    transport_drops: int
    last_csi_sequence: int
    raw_target_hz: int
    dsp_target_hz: int


@dataclass(frozen=True)
class SyncRecord(Common):
    node_id: int
    receiver_mac: bytes
    sync_version: int
    sync_state: int
    boot_id: int
    sync_sequence: int
    local_timestamp_us: int
    leader_timestamp_us: int
    raw_offset_us: int
    smoothed_offset_us: int
    drift_milli_ppm: int
    accepted_samples: int
    rejected_samples: int
    last_sync_age_us: int
    leader_node_id: int
    leader_session_id: int
    follower_session_id: int
    beacon_sequence: int
    jitter_us: int
    transport_rx: int
    transport_drops: int

    @property
    def host_timestamp_us(self) -> int:
        """Legacy compatibility alias; Phase 6 uses leader_timestamp_us."""
        return self.leader_timestamp_us


@dataclass(frozen=True)
class DiagnosticRecord(Common):
    node_id: int
    receiver_mac: bytes
    diagnostic_flags: int
    boot_id: int
    diagnostic_sequence: int
    timestamp_us: int
    status_transport_ok: int
    status_transport_drops: int
    probe_tx_attempted: int
    probe_tx_queued: int
    probe_tx_success: int
    probe_tx_link_failure: int
    probe_tx_submit_failure: int
    probe_tx_skipped_busy: int
    probe_tx_skipped_unassociated: int
    probe_rx_valid: int
    probe_rx_invalid: int
    path_1_gate_drops: int
    path_2_gate_drops: int
    path_3_gate_drops: int
    association_epoch: int


Record = Union[CsiRecord, StatusRecord, SyncRecord, DiagnosticRecord, Common]


def _validate_common(data: bytes) -> tuple[int, int, int, int]:
    if len(data) < 16:
        raise ProtocolError("truncated common header")
    magic, version, record_type, header_length, record_length, expected_crc = struct.unpack_from(
        "<4sBBHII", data
    )
    if magic != MAGIC:
        raise ProtocolError("wrong magic")
    if version != VERSION:
        raise ProtocolError(f"unsupported major version {version}")
    if record_length != len(data) or record_length > MAX_RECORD_SIZE:
        raise ProtocolError("record length does not match datagram")
    if header_length < 16 or header_length > record_length:
        raise ProtocolError("invalid header length")
    check = bytearray(data)
    check[12:16] = b"\0\0\0\0"
    if crc32c(check) != expected_crc:
        raise ProtocolError("CRC-32C mismatch")
    return record_type, header_length, record_length, expected_crc


def decode(data: bytes) -> Record:
    data = bytes(data)
    record_type, header_length, record_length, checksum = _validate_common(data)
    common = (record_type, header_length, record_length, checksum, data)
    if record_type == CSI:
        if header_length != CSI_HEADER_SIZE or record_length < CSI_HEADER_SIZE:
            raise ProtocolError("invalid CSI header geometry")
        fields = struct.unpack_from("<I6s6sIQBBBBbbBBHHHHHBB", data, 16)
        (node_id, receiver, source, sequence, timestamp_us, channel, secondary,
         bandwidth, phy, rssi, noise, antenna, ltf, driver_length, payload_length,
         subcarriers, flags, path_id, sanitized, iq_order) = fields
        (driver_timestamp, phy_rate, mcs, rx_flags, ampdu_count, rx_state,
         packet_length, driver_sequence, reserved, destination,
         trailing_reserved) = struct.unpack_from("<IBBHBBHHH6sH", data, 64)
        if reserved != 0 or trailing_reserved != 0:
            raise ProtocolError("non-zero reserved CSI field")
        payload = data[header_length:]
        if payload_length != len(payload) or driver_length != payload_length:
            raise ProtocolError("CSI payload length mismatch")
        if payload_length == 0 or payload_length & 1 or subcarriers != payload_length // 2:
            raise ProtocolError("invalid CSI I/Q geometry")
        if iq_order != 1:
            raise ProtocolError("unsupported I/Q order")
        if sanitized > min(4, payload_length):
            raise ProtocolError("invalid sanitized prefix length")
        if sanitized:
            if flags & 0x3 != 0x3 or any(payload[:sanitized]):
                raise ProtocolError("invalid first-word sanitation")
        return CsiRecord(*common, node_id, receiver, source, sequence, timestamp_us,
                         channel, secondary, bandwidth, phy, rssi, noise, antenna,
                         ltf, driver_length, payload_length, subcarriers, flags,
                         path_id, sanitized, iq_order, driver_timestamp, phy_rate,
                         mcs, rx_flags, ampdu_count, rx_state, packet_length,
                         driver_sequence, destination, payload)
    if record_type == STATUS:
        if header_length != STATUS_SIZE or record_length != STATUS_SIZE:
            raise ProtocolError("invalid STATUS length")
        fields = struct.unpack_from("<I6sHIIQIIIIIIIIHH", data, 16)
        return StatusRecord(*common, *fields)
    if record_type == SYNC:
        if header_length == LEGACY_SYNC_SIZE and record_length == LEGACY_SYNC_SIZE:
            fields = struct.unpack_from("<I6sHIIQQIII", data, 16)
            node, receiver, flags, boot, sequence, local, host, rtt, last_csi, source = fields
            return SyncRecord(*common, node, receiver, 0, 0, boot, sequence, local, host,
                              host - local, host - local, 0, 0, 0, rtt, 0, 0, boot,
                              last_csi, 0, source, 0)
        if header_length != SYNC_SIZE or record_length != SYNC_SIZE:
            raise ProtocolError("invalid SYNC length")
        fields = struct.unpack_from("<I6sBBIIQQqqi10I", data, 16)
        if fields[2] != 1 or fields[3] > 4:
            raise ProtocolError("unsupported SYNC contract or state")
        return SyncRecord(*common, *fields)
    if record_type == DIAGNOSTIC:
        if header_length != DIAGNOSTIC_SIZE or record_length != DIAGNOSTIC_SIZE:
            raise ProtocolError("invalid DIAGNOSTIC length")
        fields = struct.unpack_from("<I6sHIIQ15I", data, 16)
        return DiagnosticRecord(*common, *fields)
    return Common(*common)


def encode_sync(record: SyncRecord) -> bytes:
    """Serialize a Phase-6 SYNC record for deterministic tests/tools."""
    if record.sync_version != 1 or record.sync_state not in range(5):
        raise ProtocolError("unsupported SYNC contract or state")
    output = bytearray(SYNC_SIZE)
    struct.pack_into("<4sBBHII", output, 0, MAGIC, VERSION, SYNC,
                     SYNC_SIZE, SYNC_SIZE, 0)
    struct.pack_into(
        "<I6sBBIIQQqqi10I", output, 16,
        record.node_id, record.receiver_mac, record.sync_version,
        record.sync_state, record.boot_id, record.sync_sequence,
        record.local_timestamp_us, record.leader_timestamp_us,
        record.raw_offset_us, record.smoothed_offset_us,
        record.drift_milli_ppm, record.accepted_samples,
        record.rejected_samples, record.last_sync_age_us,
        record.leader_node_id, record.leader_session_id,
        record.follower_session_id, record.beacon_sequence,
        record.jitter_us, record.transport_rx, record.transport_drops,
    )
    struct.pack_into("<I", output, 12, crc32c(output))
    return bytes(output)
