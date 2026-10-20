"""What the playlist is looking at, and whether a change to it needs a rebuild.

The piano roll's rule (panels/piano_roll/view_state.py) on the playlist: geometry is
built for the visible range grown by one view-width either side and every track row, so
a pan inside that range is a matrix and no engine call. A rebuild is needed when the view
leaves the range or the zoom moves far enough to change what a clip draws (its level of
detail is decided in pixels at build time).
"""

from __future__ import annotations

from dataclasses import dataclass

from adx.engine_bridge import PPQ


@dataclass(frozen=True, slots=True)
class Built:
    """What the last build covered."""

    tick_start: float
    tick_end: float
    pixels_per_tick: float
    pixels_per_row: float


@dataclass(slots=True)
class PlaylistView:
    """The playlist's viewport, in the item's pixels. World x is beats; y is rows."""

    tick_start: float = 0.0
    row_start: float = 0.0
    pixels_per_tick: float = 24.0 / PPQ
    pixels_per_row: float = 44.0
    width: float = 900.0
    height: float = 400.0
    min_pixels_per_tick: float = 0.5 / PPQ
    max_pixels_per_tick: float = 400.0 / PPQ

    @property
    def tick_end(self) -> float:
        """Tick at the right edge."""
        return self.tick_start + self.width / self.pixels_per_tick

    @property
    def row_end(self) -> float:
        """Row at the bottom edge."""
        return self.row_start + self.height / self.pixels_per_row

    def tick_at(self, x: float) -> float:
        """The tick under a pixel column."""
        return self.tick_start + x / self.pixels_per_tick

    def x_of(self, tick: float) -> float:
        """The pixel column of a tick."""
        return (tick - self.tick_start) * self.pixels_per_tick

    def row_at(self, y: float) -> float:
        """The row under a pixel row."""
        return self.row_start + y / self.pixels_per_row

    def y_of(self, row: float) -> float:
        """The pixel row of a world row."""
        return (row - self.row_start) * self.pixels_per_row

    def transform(self) -> tuple[float, float, float, float]:
        """GeometryItem.setView's arguments: origin in beats and rows, scales."""
        return (
            self.tick_start / PPQ,
            self.row_start,
            self.pixels_per_tick * PPQ,
            self.pixels_per_row,
        )

    def hit_view(self) -> tuple[float, float, float, float]:
        """PlaylistGeometry.hit_test's view: (tick_start, row_start, ppt, ppr)."""
        return (self.tick_start, self.row_start, self.pixels_per_tick, self.pixels_per_row)

    def zoom(self, factor: float, anchor_x: float) -> None:
        """Zoom horizontally about a pixel column."""
        tick = self.tick_at(anchor_x)
        self.pixels_per_tick = max(
            self.min_pixels_per_tick, min(self.max_pixels_per_tick, self.pixels_per_tick * factor)
        )
        self.tick_start = max(0.0, tick - anchor_x / self.pixels_per_tick)

    def build_range(self) -> Built:
        """The range a build covers now: the view, one width either side."""
        span = self.tick_end - self.tick_start
        return Built(
            max(0.0, self.tick_start - span),
            self.tick_end + span,
            self.pixels_per_tick,
            self.pixels_per_row,
        )

    def needs_rebuild(self, built: Built | None) -> bool:
        """Whether the view has left ``built`` or drifted from its zoom."""
        if built is None:
            return True
        if self.tick_start < built.tick_start or self.tick_end > built.tick_end:
            return True
        ratio = self.pixels_per_tick / built.pixels_per_tick
        return not (0.8 <= ratio <= 1.25) or self.pixels_per_row != built.pixels_per_row
