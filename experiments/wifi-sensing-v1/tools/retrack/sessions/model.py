from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class GeometryState(str, Enum):
    READY = "READY"
    REPOSITIONING = "REPOSITIONING"
    SETTLING = "SETTLING"
    CALIBRATION_REQUIRED = "CALIBRATION_REQUIRED"
    BUILDING = "BUILDING"


@dataclass
class GeometryProfile:
    room: str
    placement: str
    state: GeometryState = GeometryState.CALIBRATION_REQUIRED
    calibration_id: str | None = None
    changed_at_ns: int | None = None
    settle_until_ns: int | None = None

    def move(self, placement: str, now_ns: int, settle_seconds: float) -> None:
        self.placement = placement.strip() or "UNSPECIFIED"
        self.state = GeometryState.REPOSITIONING
        self.calibration_id = None
        self.changed_at_ns = now_ns
        self.settle_until_ns = now_ns + int(max(0.0, settle_seconds) * 1e9)

    def update(self, now_ns: int) -> GeometryState:
        if self.state == GeometryState.REPOSITIONING:
            self.state = GeometryState.SETTLING
        if self.state == GeometryState.SETTLING and self.settle_until_ns is not None and now_ns >= self.settle_until_ns:
            self.state = GeometryState.CALIBRATION_REQUIRED
        return self.state

    def begin_calibration(self) -> None:
        if self.state not in (GeometryState.CALIBRATION_REQUIRED, GeometryState.BUILDING):
            raise ValueError("geometry must settle before calibration")
        self.state = GeometryState.BUILDING

    def ready(self, calibration_id: str) -> None:
        if not calibration_id:
            raise ValueError("calibration identity is required")
        self.calibration_id = calibration_id
        self.state = GeometryState.READY
