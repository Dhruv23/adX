"""The playlist's tools: what a press, a drag and a release on the arrangement mean
(phase_6.md 4.1: draw, paint, select, slice, slip, stretch, mute, group).

Each gesture ends in commands through the session and is one undo step: a drag is a run
of SetPlaylistItem commands the engine coalesces into one, and a gesture that makes
several commands (painting a run, slicing, grouping) opens one group for its duration.
Hit tests are the engine's (PlaylistGeometry.hit_test); placements are read fresh from
the engine on every gesture, never kept.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, ClassVar

import numpy as np

from adx.panels.geometry_view import Pointer
from adx.selection import Context, Kind

if TYPE_CHECKING:
    from adx.panels.playlist.panel import PlaylistPanel

#: PlaylistGeometry ClipPart values.
NONE, BODY, LEFT_EDGE, RIGHT_EDGE, HEADER = 0, 1, 2, 3, 4


@dataclass(slots=True)
class Grab:
    """What a press took hold of."""

    track: int
    item: int
    part: int
    x: float
    y: float
    start: int
    length: int
    offset: int
    stretch: float
    moved: bool = False


class Tool:
    """A tool: subclasses override what they need."""

    name: ClassVar[str] = "tool"
    label: ClassVar[str] = "Tool"
    shortcut: ClassVar[str] = ""

    def __init__(self, panel: PlaylistPanel) -> None:
        self.panel = panel
        self.grab: Grab | None = None
        self.band: tuple[float, float, float, float] | None = None

    def press(self, pointer: Pointer) -> None:
        """A button went down."""

    def move(self, pointer: Pointer) -> None:
        """The pointer moved with a button down."""

    def release(self, pointer: Pointer) -> None:
        """The button came up."""

    # --- shared ---------------------------------------------------------------------

    def take(self, pointer: Pointer) -> Grab | None:
        """Hit-test and, on a clip, remember it as it is now."""
        track, item, part = self.panel.hit(pointer.x, pointer.y)
        if not item:
            return None
        row = self.panel.item_row(item)
        if row is None:
            return None
        return Grab(
            track=track,
            item=item,
            part=part,
            x=pointer.x,
            y=pointer.y,
            start=int(row["start"]),
            length=int(row["length"]),
            offset=int(row["offset"]),
            stretch=float(row["stretch"]),
        )

    def select_clip(self, item: int, add: bool = False) -> None:
        """Make ``item`` the selection (or add it)."""
        current = list(self.panel.selection.ids(Kind.CLIPS)) if add else []
        if item not in current:
            current.append(item)
        self.panel.selection.select(Kind.CLIPS, current, Context())

    def drag_ticks(self, pointer: Pointer) -> int:
        """How far the pointer has moved since the press, in snapped ticks."""
        assert self.grab is not None
        delta = (pointer.x - self.grab.x) / self.panel.view.pixels_per_tick
        return self.panel.snap_delta(delta)


class DrawTool(Tool):
    """Click empty space to place the chosen pattern; drag a clip to move it, its
    edges to trim it."""

    name = "draw"
    label = "Draw"
    shortcut = "P"

    def press(self, pointer: Pointer) -> None:
        self.grab = self.take(pointer)
        if self.grab is None:
            self.panel.place_pattern(pointer)
            return
        self.select_clip(self.grab.item, pointer.shift)

    def move(self, pointer: Pointer) -> None:
        grab = self.grab
        if grab is None:
            return
        grab.moved = True
        delta = self.drag_ticks(pointer)
        if grab.part == LEFT_EDGE:
            cut = max(-grab.offset, min(delta, grab.length - self.panel.min_length()))
            self.panel.edit_item(
                grab.track,
                grab.item,
                {
                    "start": grab.start + cut,
                    "offset": grab.offset + cut,
                    "length": grab.length - cut,
                },
            )
        elif grab.part == RIGHT_EDGE:
            length = max(self.panel.min_length(), grab.length + delta)
            self.panel.edit_item(grab.track, grab.item, {"length": length})
        else:
            fields: dict[str, float] = {"start": max(0, grab.start + delta)}
            track = self.panel.track_at(pointer.y)
            if track and track != grab.track:
                fields["track"] = track
            self.panel.edit_item(grab.track, grab.item, fields)
            if track:
                grab.track = track

    def release(self, pointer: Pointer) -> None:
        self.grab = None


class PaintTool(Tool):
    """Drag across empty space to lay the chosen pattern down end to end: one step."""

    name = "paint"
    label = "Paint"
    shortcut = "B"

    def __init__(self, panel: PlaylistPanel) -> None:
        super().__init__(panel)
        self._painted: set[int] = set()
        self._track = 0

    def press(self, pointer: Pointer) -> None:
        if self.panel.hit(pointer.x, pointer.y)[1]:
            return
        self._track = self.panel.track_at(pointer.y)
        if not self._track:
            return
        self.panel.session.begin_gesture("Paint clips")
        self._painted = set()
        self._paint(pointer)

    def _paint(self, pointer: Pointer) -> None:
        length = self.panel.pattern_length()
        if length <= 0:
            return
        slot = int(max(0.0, self.panel.view.tick_at(pointer.x)) // length)
        if slot not in self._painted:
            self._painted.add(slot)
            self.panel.add_pattern_at(self._track, slot * length)

    def move(self, pointer: Pointer) -> None:
        if self._track:
            self._paint(pointer)

    def release(self, pointer: Pointer) -> None:
        if self._track:
            self.panel.session.end_gesture()
        self._track = 0


class SelectTool(Tool):
    """Click to select, drag a band to select many, drag a selected clip to move every
    selected clip together."""

    name = "select"
    label = "Select"
    shortcut = "E"

    def __init__(self, panel: PlaylistPanel) -> None:
        super().__init__(panel)
        self._moving: list[Grab] = []

    def press(self, pointer: Pointer) -> None:
        self.grab = self.take(pointer)
        if self.grab is None:
            self.band = (pointer.x, pointer.y, pointer.x, pointer.y)
            return
        selected = self.panel.selection.ids(Kind.CLIPS)
        if self.grab.item not in selected or pointer.shift:
            self.select_clip(self.grab.item, pointer.shift)
        self._moving = []
        for item in self.panel.selection.ids(Kind.CLIPS):
            row = self.panel.item_row(item)
            if row is not None:
                self._moving.append(
                    Grab(
                        int(row["track"]),
                        item,
                        BODY,
                        pointer.x,
                        pointer.y,
                        int(row["start"]),
                        int(row["length"]),
                        int(row["offset"]),
                        float(row["stretch"]),
                    )
                )
        self.panel.session.begin_gesture("Move clips")

    def move(self, pointer: Pointer) -> None:
        if self.band is not None:
            x0, y0, _x1, _y1 = self.band
            self.band = (x0, y0, pointer.x, pointer.y)
            self.panel.show_band(self.band)
            return
        if self.grab is None:
            return
        delta = self.drag_ticks(pointer)
        for grab in self._moving:
            self.panel.edit_item(grab.track, grab.item, {"start": max(0, grab.start + delta)})

    def release(self, pointer: Pointer) -> None:
        if self.band is not None:
            band = self.band
            self.band = None
            self.panel.show_band(None)
            self.panel.select_band(band, pointer.shift)
            return
        if self.grab is not None:
            self.panel.session.end_gesture()
        self.grab = None
        self._moving = []


class SliceTool(Tool):
    """Click a clip to cut it in two at the snapped position: one step."""

    name = "slice"
    label = "Slice"
    shortcut = "C"

    def press(self, pointer: Pointer) -> None:
        grab = self.take(pointer)
        if grab is None:
            return
        at = self.panel.snap_tick(self.panel.view.tick_at(pointer.x))
        self.panel.slice(grab.track, grab.item, at)


class SlipTool(Tool):
    """Drag a clip's content within it: the source offset moves, the clip does not."""

    name = "slip"
    label = "Slip"
    shortcut = "S"

    def press(self, pointer: Pointer) -> None:
        self.grab = self.take(pointer)

    def move(self, pointer: Pointer) -> None:
        if self.grab is None:
            return
        offset = max(0, self.grab.offset - self.drag_ticks(pointer))
        self.panel.edit_item(self.grab.track, self.grab.item, {"offset": offset})

    def release(self, pointer: Pointer) -> None:
        self.grab = None


class StretchTool(Tool):
    """Drag a clip's end: its length changes, and an audio clip's stretch with it, so
    the same audio fills the new length."""

    name = "stretch"
    label = "Stretch"
    shortcut = "X"

    def press(self, pointer: Pointer) -> None:
        self.grab = self.take(pointer)

    def move(self, pointer: Pointer) -> None:
        grab = self.grab
        if grab is None:
            return
        length = max(self.panel.min_length(), grab.length + self.drag_ticks(pointer))
        fields: dict[str, float] = {"length": length}
        if self.panel.item_kind(grab.item) == 1:
            fields["stretch"] = grab.stretch * length / max(1, grab.length)
        self.panel.edit_item(grab.track, grab.item, fields)

    def release(self, pointer: Pointer) -> None:
        self.grab = None


class MuteTool(Tool):
    """Click a clip to mute or unmute it."""

    name = "mute"
    label = "Mute"
    shortcut = "T"

    def press(self, pointer: Pointer) -> None:
        grab = self.take(pointer)
        if grab is None:
            return
        row = self.panel.item_row(grab.item)
        if row is not None:
            muted = 0.0 if int(row["muted"]) else 1.0
            self.panel.edit_item(grab.track, grab.item, {"muted": muted})


class GroupTool(Tool):
    """Drag down across tracks: the ones below the first join its group (one step)."""

    name = "group"
    label = "Group"
    shortcut = "G"

    def __init__(self, panel: PlaylistPanel) -> None:
        super().__init__(panel)
        self._first = 0
        self._y = 0.0

    def press(self, pointer: Pointer) -> None:
        self._first = self.panel.track_at(pointer.y)
        self._y = pointer.y

    def release(self, pointer: Pointer) -> None:
        if not self._first:
            return
        rows = self.panel.track_rows()
        low, high = sorted((self._y, pointer.y))
        members = [
            track
            for track, top, height in rows
            if track != self._first
            and height > 0
            and self.panel.view.y_of(top + height) > low
            and self.panel.view.y_of(top) < high
        ]
        if members:
            self.panel.group_tracks(self._first, members)
        self._first = 0


TOOLS: tuple[type[Tool], ...] = (
    DrawTool,
    PaintTool,
    SelectTool,
    SliceTool,
    SlipTool,
    StretchTool,
    MuteTool,
    GroupTool,
)


def band_items(
    visible: np.ndarray,
    band: tuple[float, float, float, float],
    view: tuple[float, float, float, float],
) -> list[int]:
    """Item ids whose drawn rectangles a pixel band touches. ``visible`` is
    PlaylistGeometry.visible(); ``view`` is (tick_start, row_start, ppt, ppr)."""
    if len(visible) == 0:
        return []
    tick_start, row_start, ppt, ppr = view
    x0, x1 = sorted((band[0], band[2]))
    y0, y1 = sorted((band[1], band[3]))
    left = (visible[:, 2] - tick_start) * ppt
    right = (visible[:, 3] - tick_start) * ppt
    top = (visible[:, 4] - row_start) * ppr
    bottom = (visible[:, 4] + visible[:, 5] - row_start) * ppr
    hit = (right >= x0) & (left <= x1) & (bottom >= y0) & (top <= y1)
    return [int(i) for i in visible[hit, 0]]
