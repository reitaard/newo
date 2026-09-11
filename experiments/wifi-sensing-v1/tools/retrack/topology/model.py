from __future__ import annotations

from dataclasses import asdict, dataclass


@dataclass
class Link:
    link_id: str
    source: str
    receiver: str
    directed: bool = True
    kind: str = "CSI"
    enabled: bool = True
    health: str = "UNKNOWN"
    last_seen_ns: int | None = None


class Topology:
    def __init__(self) -> None:
        self._links: dict[str, Link] = {}

    def upsert(self, link: Link) -> Link:
        self._links[link.link_id] = link
        return link

    @property
    def links(self) -> tuple[Link, ...]:
        return tuple(sorted(self._links.values(), key=lambda item: item.link_id))

    def as_list(self) -> list[dict[str, object]]:
        return [asdict(link) for link in self.links]
