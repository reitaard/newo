"""Small host announcement contract for future collector discovery.

Firmware does not consume this yet.  The pure selection function documents and
tests precedence without changing either ESP target.
"""

from __future__ import annotations

from dataclasses import dataclass
import ipaddress
import struct

MAGIC = b"NCOL"
VERSION = 1
ANNOUNCEMENT_PORT = 47777
MULTICAST_GROUP = "239.255.77.77"
ANNOUNCEMENT = struct.Struct("<4sBBHHI4s")


@dataclass(frozen=True)
class CollectorAddress:
    host: str
    port: int
    source: str


def encode_announcement(data_port: int, lease_seconds: int = 15,
                        nonce: int = 0) -> bytes:
    if not 1 <= data_port <= 65535:
        raise ValueError("data port must be 1..65535")
    if not 5 <= lease_seconds <= 300:
        raise ValueError("lease must be 5..300 seconds")
    return ANNOUNCEMENT.pack(MAGIC, VERSION, 0, data_port, lease_seconds,
                             nonce & 0xFFFFFFFF, b"\0\0\0\0")


def decode_announcement(payload: bytes, sender_ip: str) -> CollectorAddress:
    if len(payload) != ANNOUNCEMENT.size:
        raise ValueError("wrong collector announcement length")
    magic, version, flags, port, lease, _nonce, reserved = ANNOUNCEMENT.unpack(payload)
    if magic != MAGIC or version != VERSION or flags != 0 or reserved != b"\0" * 4:
        raise ValueError("invalid collector announcement")
    if not 1 <= port <= 65535 or not 5 <= lease <= 300:
        raise ValueError("invalid collector announcement values")
    address = ipaddress.ip_address(sender_ip)
    if address.version != 4 or address.is_multicast or address.is_unspecified:
        raise ValueError("invalid collector sender address")
    return CollectorAddress(str(address), port, "announcement")


def select_collector_address(explicit: tuple[str, int] | None,
                             discovered: CollectorAddress | None,
                             configured: tuple[str, int] | None) -> CollectorAddress:
    """Explicit runtime override, then fresh discovery, then firmware fallback."""
    if explicit is not None:
        return CollectorAddress(str(ipaddress.ip_address(explicit[0])), explicit[1], "explicit")
    if discovered is not None:
        return discovered
    if configured is not None:
        return CollectorAddress(str(ipaddress.ip_address(configured[0])), configured[1], "configured")
    raise ValueError("no collector address available")
