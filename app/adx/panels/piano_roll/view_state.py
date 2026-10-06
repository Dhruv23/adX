"""What the piano roll is looking at, and whether a change to it needs a rebuild.

Python owns the viewport (phase_5.md 4.2): where the view is, how far it is zoomed.
Turning a pan or zoom into pixels is a transform the scene-graph item applies, so a pan
costs a few float operations here and **no engine call at all** - which
``test_perf_geometry.py`` asserts as a count.

Geometry is built for the visible rectangle grown by one view-width either side, at the
current zoom. A rebuild is needed only when the view leaves that range, when a zoom
crosses the density level of detail, or when it drifts more than 2x from the zoom the
build was made at (note gaps and lane bars are sized in pixels at build time).
"""

from __future__ import annotations

from dataclasses import dataclass

from adx.engine_bridge import DENSITY_PIXELS_PER_SIXTEENTH, PPQ

#: Pitch rows: world y = 128 - pitch value.
ROWS = 128.0


@dataclass(frozen=True, slots=True)
class BuiltRange:
    """What the last geometry build covered."""

    tick_start: float
    tick_end: float
    pixels_per_tick: float
    density: bool


@dataclass(slots=True)
class ViewState:
    """The piano roll's viewport, in the item's pixels."""

    #: Tick at the left edge.
    tick_start: float = 0.0
    #: Pitch value at the top edge (row p covers values p .. p+1).
    pitch_top: float = 84.0
    pixels_per_tick: float = 48.0 / PPQ
    pixels_per_semitone: float = 14.0
    width: float = 800.0
    #: The item's full height; the lane strip takes the bottom :attr:`lane_height`.
    height: float = 500.0
    lane_height: float = 84.0
    min_pixels_per_tick: float = 0.2 / PPQ
    max_pixels_per_tick: float = 4000.0 / PPQ

    @property
    def note_height(self) -> float:
        """Pixels of the note area, above the lane strip."""
        return max(1.0, self.height - self.lane_height)

    @property
    def tick_end(self) -> float:
        """Tick at the right edge."""
        return self.tick_start + self.width / self.pixels_per_tick

    @property
    def pitch_bottom(self) -> float:
        """Pitch value at the bottom of the note area."""
        return self.pitch_top - self.note_height / self.pixels_per_semitone

    @property
    def density(self) -> bool:
        """Whether this zoom draws notes as density runs (Viewport.h's densityLod)."""
        return self.pixels_per_tick * (PPQ // 4) < DENSITY_PIXELS_PER_SIXTEENTH

    def tick_at(self, x: float) -> float:
        """The tick under pixel column x."""
        return self.tick_start + x / self.pixels_per_tick

    def pitch_value_at(self, y: float) -> float:
        """The pitch value under pixel row y (floor it for the row)."""
        return self.pitch_top - y / self.pixels_per_semitone

    def row_at(self, y: float) -> int:
        """The pitch row under pixel row y, clamped to MIDI."""
        return max(0, min(127, int(self.pitch_value_at(y) // 1)))

    def x_of(self, tick: float) -> float:
        """Pixel column of a tick."""
        return (tick - self.tick_start) * self.pixels_per_tick

    def y_of(self, pitch_value: float) -> float:
        """Pixel row of a pitch value."""
        return (self.pitch_top - pitch_value) * self.pixels_per_semitone

    def in_lane(self, y: float) -> bool:
        """Whether pixel row y is in the lane strip."""
        return y >= self.note_height

    # --- the transform the item applies ---------------------------------------------

    def transform(self) -> tuple[float, float, float, float]:
        """(beat_start, world_top, pixels_per_beat, pixels_per_row) for setView()."""
        return (
            self.tick_start / PPQ,
            ROWS - self.pitch_top,
            self.pixels_per_tick * PPQ,
            self.pixels_per_semitone,
        )

    # --- interaction ----------------------------------------------------------------

    def pan(self, dx: float, dy: float) -> None:
        """Move the view by pixels (positive dx shows later time)."""
        self.tick_start = max(
            -self.width / self.pixels_per_tick * 0.25, self.tick_start + dx / self.pixels_per_tick
        )
        top = self.pitch_top + dy / self.pixels_per_semitone
        visible = self.note_height / self.pixels_per_semitone
        self.pitch_top = max(min(top, ROWS), min(ROWS, visible))

    def zoom_time(self, factor: float, anchor_x: float) -> None:
        """Zoom horizontally about a pixel column."""
        anchor = self.tick_at(anchor_x)
        self.pixels_per_tick = max(
            self.min_pixels_per_tick, min(self.max_pixels_per_tick, self.pixels_per_tick * factor)
        )
        self.tick_start = anchor - anchor_x / self.pixels_per_tick

    def zoom_pitch(self, factor: float, anchor_y: float) -> None:
        """Zoom vertically about a pixel row."""
        anchor = self.pitch_value_at(anchor_y)
        self.pixels_per_semitone = max(4.0, min(48.0, self.pixels_per_semitone * factor))
        self.pitch_top = min(ROWS, anchor + anchor_y / self.pixels_per_semitone)

    def show_ticks(self, start: float, end: float) -> None:
        """Fit a tick range to the width."""
        span = max(1.0, end - start)
        self.pixels_per_tick = max(
            self.min_pixels_per_tick, min(self.max_pixels_per_tick, self.width / span)
        )
        self.tick_start = start

    def center_pitch(self, pitch: float) -> None:
        """Scroll so ``pitch`` is mid-view."""
        self.pitch_top = min(ROWS, pitch + self.note_height / self.pixels_per_semitone / 2)

    # --- when to rebuild --------------------------------------------------------------

    def build_range(self) -> BuiltRange:
        """What a build made now should cover: the view, one width either side."""
        span = self.tick_end - self.tick_start
        return BuiltRange(
            tick_start=self.tick_start - span,
            tick_end=self.tick_end + span,
            pixels_per_tick=self.pixels_per_tick,
            density=self.density,
        )

    def needs_rebuild(self, built: BuiltRange | None) -> bool:
        """Whether the current view is outside what ``built`` covers."""
        if built is None:
            return True
        if self.density != built.density:
            return True
        ratio = self.pixels_per_tick / built.pixels_per_tick
        if ratio > 2.0 or ratio < 0.5:
            return True
        return self.tick_start < built.tick_start or self.tick_end > built.tick_end
