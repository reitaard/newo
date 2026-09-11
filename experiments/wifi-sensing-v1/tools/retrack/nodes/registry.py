from __future__ import annotations

from dataclasses import asdict, dataclass, field
import json
from pathlib import Path
import re
from typing import Iterable


def normalize_mac(value: str) -> str:
    compact = re.sub(r"[^0-9A-Fa-f]", "", value)
    if len(compact) != 12:
        raise ValueError(f"invalid hardware MAC: {value}")
    return ":".join(compact[index:index + 2] for index in range(0, 12, 2)).lower()


@dataclass
class Node:
    node_id: str
    friendly_name: str
    hardware_mac: str
    device_type: str = "ESP32"
    role: str = "FOLLOWER"
    capabilities: list[str] = field(default_factory=list)
    firmware: str | None = None
    room: str | None = None
    placement: str | None = None
    last_ip: str | None = None
    last_seen_ns: int | None = None
    sync_role: str = "NONE"
    sensing: dict[str, object] = field(default_factory=dict)
    boot_session_id: int | None = None


class NodeRegistry:
    """Dynamically sized persistent registry keyed by stable hardware identity."""

    def __init__(self, path: Path | None = None):
        self.path = path
        self._nodes: dict[str, Node] = {}
        if path and path.is_file():
            document = json.loads(path.read_text(encoding="utf-8"))
            if document.get("schema") != "retrack_node_registry_v1":
                raise ValueError("unsupported ReTrack node registry")
            for row in document.get("nodes", []):
                node = Node(**row)
                node.hardware_mac = normalize_mac(node.hardware_mac)
                self._nodes[node.hardware_mac] = node

    @property
    def nodes(self) -> tuple[Node, ...]:
        return tuple(sorted(self._nodes.values(), key=lambda item: item.node_id))

    def _next_id(self) -> str:
        used = {int(match.group(1)) for item in self._nodes.values()
                if (match := re.fullmatch(r"RT-N(\d{3,})", item.node_id))}
        value = 1
        while value in used:
            value += 1
        return f"RT-N{value:03d}"

    def register(self, hardware_mac: str, *, friendly_name: str | None = None,
                 device_type: str = "ESP32", role: str = "FOLLOWER",
                 capabilities: Iterable[str] = (), last_ip: str | None = None,
                 last_seen_ns: int | None = None, firmware: str | None = None,
                 room: str | None = None, placement: str | None = None,
                 sync_role: str | None = None, boot_session_id: int | None = None) -> Node:
        mac = normalize_mac(hardware_mac)
        node = self._nodes.get(mac)
        if node is None:
            node = Node(self._next_id(), friendly_name or f"Node {len(self._nodes) + 1}", mac)
            self._nodes[mac] = node
        if friendly_name is not None:
            node.friendly_name = friendly_name
        node.device_type = device_type or node.device_type
        node.role = role or node.role
        node.capabilities = sorted(set(node.capabilities).union(str(item) for item in capabilities))
        if last_ip is not None:
            node.last_ip = last_ip
        if last_seen_ns is not None:
            node.last_seen_ns = last_seen_ns
        if firmware is not None:
            node.firmware = firmware
        if room is not None:
            node.room = room
        if placement is not None:
            node.placement = placement
        if sync_role is not None:
            node.sync_role = sync_role
        if boot_session_id is not None:
            node.boot_session_id = boot_session_id
        self.save()
        return node

    def get_by_mac(self, hardware_mac: str) -> Node | None:
        return self._nodes.get(normalize_mac(hardware_mac))

    def visible_slots(self, count: int = 4) -> list[Node | None]:
        if count < 1:
            raise ValueError("slot count must be positive")
        return list(self.nodes[:count]) + [None] * max(0, count - len(self._nodes))

    def save(self) -> None:
        if self.path is None:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_suffix(self.path.suffix + ".tmp")
        temporary.write_text(json.dumps({"schema": "retrack_node_registry_v1",
                                         "nodes": [asdict(node) for node in self.nodes]},
                                        indent=2, sort_keys=True) + "\n", encoding="utf-8")
        temporary.replace(self.path)
