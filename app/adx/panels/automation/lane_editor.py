"""One automation lane, edited (phase_6.md 4.5).

The lane is drawn by AutomationGeometry, which tessellates every segment with the
engine's own core::Curve - the UI never computes a curve. Editing:

- click empty space: a point there (values snapped when snapping is on);
- drag a point: move it; shift-click to add points to the selection;
- drag empty space: select the points in a band;
- with points selected, drag one of them: move them all; ctrl-drag: scale their values
  about their middle;
- alt-drag a segment: bend it (its tension);
- Delete: remove the selected points;
- a shape from the library (ramp, pulse, sine, random) over the selection or one bar.

Every gesture ends in SetBreakpoints - consecutive ones on a lane coalesce, so a drag of
any number of points is one undo step (``automation_drag_is_one_command``). A lane may
be a clip's (SetBreakpoints) or a placement's envelope (SetClipEnvelopes): ``LaneSource``
hides which.
"""

from __future__ import annotations

import random
from collections.abc import Callable
from dataclasses import dataclass

import numpy as np
import numpy.typing as npt

from adx import engine_bridge
from adx.engine_bridge import PPQ, AutomationGeometry
from adx.panels.geometry_view import (
    ALT,
    CTRL,
    WORLD_LINES,
    WORLD_TRIANGLES,
    GeometryView,
    Pointer,
)

Points = npt.NDArray[np.float64]


@dataclass(slots=True)
class LaneSource:
    """Where a lane's points come from and go to."""

    label: str
    #: Read the points now: (n, 8) tick, value, kind, tension, c1x, c1y, c2x, c2y.
    read: Callable[[], Points]
    #: Replace them: one command.
    write: Callable[[Points], None]
    #: (minimum, maximum, logarithmic).
    lane: tuple[float, float, bool]


def shape_points(
    shape: str, start: float, end: float, low: float, high: float, seed: int = 1
) -> Points:
    """A shape from the library over [start, end] ticks, values between low and high."""
    span = max(end - start, 1.0)
    if shape == "ramp":
        rows = [(start, low, 0), (end, high, 0)]
    elif shape == "pulse":
        half = start + span / 2
        rows = [(start, high, 3), (half, low, 3), (end, high, 3)]
    elif shape == "sine":
        steps = 8
        rows = [
            (
                start + span * i / steps,
                low + (high - low) * (0.5 + 0.5 * np.sin(2 * np.pi * i / steps)),
                4,
            )
            for i in range(steps + 1)
        ]
    else:
        rng = random.Random(seed)
        steps = 8
        rows = [
            (start + span * i / steps, low + (high - low) * rng.random(), 3)
            for i in range(steps + 1)
        ]
    out = np.zeros((len(rows), 8), dtype=np.float64)
    for index, (tick, value, kind) in enumerate(rows):
        out[index, :4] = (tick, value, kind, 0.0)
        out[index, 4:] = (0.33, 0.0, 0.67, 1.0)
    return out


class LaneEditor:
    """A lane view and what input on it means."""

    HEIGHT = 160.0

    def __init__(self) -> None:
        self.source: LaneSource | None = None
        self.geometry = AutomationGeometry()
        self.view = GeometryView(self, (WORLD_LINES, WORLD_TRIANGLES))
        self.tick_start = 0.0
        self.pixels_per_tick = 32.0 / PPQ
        self.width = 600.0
        self.height = self.HEIGHT
        self.snap_ticks = PPQ // 4
        self.snap_values = False
        self.selected: set[int] = set()
        self.writes = 0
        self._grab: tuple[str, int, float, float, Points] | None = None
        self._band: tuple[float, float, float, float] | None = None

    # --- coordinates ----------------------------------------------------------------

    def tick_at(self, x: float) -> float:
        return self.tick_start + x / self.pixels_per_tick

    def value_at(self, y: float) -> float:
        assert self.source is not None
        position = 1.0 - max(0.0, min(1.0, y / self.height))
        value = engine_bridge.lane_denormalise(position, self.source.lane)
        if self.snap_values:
            low, high, _log = self.source.lane
            step = (high - low) / 16.0
            value = round((value - low) / step) * step + low if step > 0 else value
        return value

    def snap(self, tick: float) -> float:
        return (
            float(max(0, round(tick / self.snap_ticks) * self.snap_ticks))
            if self.snap_ticks
            else max(0.0, tick)
        )

    # --- geometry -------------------------------------------------------------------

    def show(self, source: LaneSource | None) -> None:
        """Edit ``source`` (or nothing)."""
        # The panel rebuilds its sources after every change, so "the same lane" is the
        # same label, not the same object - or a drag would lose its selection at the
        # first write.
        if source is None or self.source is None or source.label != self.source.label:
            self.selected = set()
        self.source = source
        self.rebuild()

    def rebuild(self) -> None:
        """Tessellate through the engine and upload."""
        if self.source is None:
            self.view.clear()
            return
        points = self.source.read()
        flags = np.zeros(len(points), dtype=np.uint8)
        for index in self.selected:
            if 0 <= index < len(points):
                flags[index] = 1
        tick_end = self.tick_start + self.width / self.pixels_per_tick
        self.geometry.build(
            points,
            self.source.lane,
            (self.tick_start, tick_end, self.pixels_per_tick, self.height),
            flags,
        )
        for layer in (0, 1):
            with self.geometry.view(layer) as lease:
                self.view.upload(layer, lease)
        self.view.set_view(self.tick_start / PPQ, 0.0, self.pixels_per_tick * PPQ, self.height)

    def write(self, points: Points) -> None:
        """One command (SetBreakpoints coalesces consecutive ones on a lane)."""
        assert self.source is not None
        order = np.argsort(points[:, 0], kind="stable")
        self.source.write(points[order])
        self.writes += 1

    # --- input ----------------------------------------------------------------------

    def hit_point(self, x: float, y: float) -> int:
        assert self.source is not None
        return engine_bridge.hit_test_lane_point(
            self.source.read(),
            self.source.lane,
            self.tick_start,
            self.pixels_per_tick,
            self.height,
            x,
            y,
        )

    def on_press(self, pointer: Pointer) -> None:
        if self.source is None:
            return
        points = self.source.read()
        index = self.hit_point(pointer.x, pointer.y)
        if pointer.modifiers & ALT:
            segment = int(np.searchsorted(points[:, 0], self.tick_at(pointer.x))) - 1
            if 0 <= segment < len(points) - 1:
                self._grab = ("bend", segment, pointer.x, pointer.y, points.copy())
            return
        if index >= 0:
            if pointer.shift:
                self.selected ^= {index}
            elif index not in self.selected:
                self.selected = {index}
            mode = "scale" if pointer.modifiers & CTRL else "move"
            self._grab = (mode, index, pointer.x, pointer.y, points.copy())
            self.rebuild()
            return
        if pointer.shift:
            self._band = (pointer.x, pointer.y, pointer.x, pointer.y)
            return
        row = np.array(
            [
                [
                    self.snap(self.tick_at(pointer.x)),
                    self.value_at(pointer.y),
                    0,
                    0,
                    0.33,
                    0.0,
                    0.67,
                    1.0,
                ]
            ]
        )
        self.write(np.concatenate([points, row]) if len(points) else row)
        self.selected = set()

    def on_move(self, pointer: Pointer) -> None:
        if self._band is not None:
            x0, y0, _x, _y = self._band
            self._band = (x0, y0, pointer.x, pointer.y)
            self.view.set_selection_rect(self._band)
            return
        if self._grab is None or self.source is None:
            return
        mode, index, x0, y0, original = self._grab
        points = original.copy()
        if mode == "bend":
            points[index, 2] = 1 if points[index, 2] == 0 else points[index, 2]
            points[index, 3] = float(
                np.clip(original[index, 3] + (y0 - pointer.y) / 80.0, -1.0, 1.0)
            )
        elif mode == "scale":
            chosen = sorted(self.selected)
            middle = float(np.mean(original[chosen, 1]))
            factor = max(0.0, 1.0 + (y0 - pointer.y) / 100.0)
            points[chosen, 1] = middle + (original[chosen, 1] - middle) * factor
        else:
            dtick = self.snap(self.tick_at(pointer.x)) - self.snap(self.tick_at(x0))
            dvalue = self.value_at(pointer.y) - self.value_at(y0)
            for i in sorted(self.selected):
                points[i, 0] = max(0.0, original[i, 0] + dtick)
                points[i, 1] = original[i, 1] + dvalue
        low, high, _log = self.source.lane
        points[:, 1] = np.clip(points[:, 1], min(low, high), max(low, high))
        self.write(points)
        self.rebuild()

    def on_release(self, pointer: Pointer) -> None:
        if self._band is not None:
            x0, y0, x1, y1 = self._band
            self._band = None
            self.view.set_selection_rect(None)
            self.select_band(min(x0, x1), max(x0, x1), min(y0, y1), max(y0, y1))
        self._grab = None

    def select_band(self, x0: float, x1: float, y0: float, y1: float) -> None:
        """Select the points inside a pixel band."""
        assert self.source is not None
        points = self.source.read()
        xs = (points[:, 0] - self.tick_start) * self.pixels_per_tick
        ys = (1.0 - engine_bridge.lane_positions(points[:, 1], self.source.lane)) * self.height
        self.selected = {
            int(i) for i in np.nonzero((xs >= x0) & (xs <= x1) & (ys >= y0) & (ys <= y1))[0]
        }
        self.rebuild()

    def delete_selected(self) -> None:
        """Remove the selected points: one command."""
        if self.source is None or not self.selected:
            return
        points = self.source.read()
        self.write(np.delete(points, sorted(self.selected), axis=0))
        self.selected = set()
        self.rebuild()

    def apply_shape(self, shape: str) -> None:
        """Replace the selection's span (or the first bar) with a library shape."""
        if self.source is None:
            return
        points = self.source.read()
        if len(self.selected) >= 2:
            chosen = sorted(self.selected)
            start, end = float(points[chosen[0], 0]), float(points[chosen[-1], 0])
        else:
            start = self.snap(self.tick_start)
            end = start + PPQ * 4
        low, high, _log = self.source.lane
        keep = points[(points[:, 0] < start) | (points[:, 0] > end)]
        self.write(np.concatenate([keep, shape_points(shape, start, end, low, high)]))
        self.selected = set()
        self.rebuild()

    def on_double_click(self, pointer: Pointer) -> None:
        index = self.hit_point(pointer.x, pointer.y) if self.source is not None else -1
        if index >= 0:
            self.selected = {index}
            self.delete_selected()

    def on_wheel(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None:
        if modifiers & CTRL:
            tick = self.tick_at(x)
            self.pixels_per_tick = max(
                1.0 / PPQ, min(400.0 / PPQ, self.pixels_per_tick * (1.15 if dy > 0 else 1 / 1.15))
            )
            self.tick_start = max(0.0, tick - x / self.pixels_per_tick)
        else:
            self.tick_start = max(0.0, self.tick_start - dy / self.pixels_per_tick * 0.3)
        self.rebuild()

    def on_resize(self, width: float, height: float) -> None:
        self.width, self.height = max(1.0, width), max(1.0, height)
        self.rebuild()

    def on_key(self, key: int, modifiers: int) -> None:
        from PySide6.QtCore import Qt

        if key in (int(Qt.Key.Key_Delete.value), int(Qt.Key.Key_Backspace.value)):
            self.delete_selected()
