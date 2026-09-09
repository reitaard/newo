"""Append-only NCSI archive envelope; embedded radio records remain unchanged."""

from __future__ import annotations

from dataclasses import dataclass
import ipaddress
from pathlib import Path
import struct
from typing import BinaryIO, Iterator

ARCHIVE_MAGIC = b"NCAP"
ARCHIVE_VERSION = 1
ENVELOPE = struct.Struct("<4sBBHQI4sHH")


class ArchiveError(ValueError):
    pass


@dataclass(frozen=True)
class ArchivedRecord:
    host_received_ns: int
    source_ip: str
    source_port: int
    record: bytes


class ArchiveWriter:
    def __init__(self, path: Path):
        self._file: BinaryIO = path.open("xb")

    def append(self, record: bytes, host_received_ns: int,
               source: tuple[str, int]) -> None:
        ip = ipaddress.IPv4Address(source[0]).packed
        self._file.write(ENVELOPE.pack(ARCHIVE_MAGIC, ARCHIVE_VERSION, 0,
                                      ENVELOPE.size, host_received_ns,
                                      len(record), ip, source[1], 0))
        self._file.write(record)

    def flush(self) -> None:
        self._file.flush()

    def close(self) -> None:
        self._file.close()

    def __enter__(self) -> "ArchiveWriter":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def iter_archive(path: Path) -> Iterator[ArchivedRecord]:
    with path.open("rb") as stream:
        while True:
            header = stream.read(ENVELOPE.size)
            if not header:
                return
            if len(header) != ENVELOPE.size:
                raise ArchiveError("truncated archive envelope")
            magic, version, flags, header_size, received_ns, length, ip, port, reserved = (
                ENVELOPE.unpack(header)
            )
            if magic != ARCHIVE_MAGIC or version != ARCHIVE_VERSION:
                raise ArchiveError("unsupported archive envelope")
            if flags != 0 or reserved != 0 or header_size != ENVELOPE.size:
                raise ArchiveError("invalid archive envelope fields")
            if length < 16 or length > 4096:
                raise ArchiveError("invalid embedded record length")
            record = stream.read(length)
            if len(record) != length:
                raise ArchiveError("truncated embedded record")
            yield ArchivedRecord(received_ns, str(ipaddress.IPv4Address(ip)), port, record)
