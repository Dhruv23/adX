"""The piano roll's editable state, without Qt (phase_5.md 4.7).

The tools act on a :class:`RollModel`: one channel's notes in one pattern, a
selection, the snap grid and scale. It holds the clip as one structured numpy array
(re-read with one engine call when the project's revision moves) and funnels every
edit through :meth:`RollModel.commit`, which executes exactly one command - so every
gesture is one undo step, and ``commands`` lets a test count them.

Kept free of Qt so the tool logic can be tested headlessly and fast.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field

import numpy as np
import numpy.typing as npt

from adx import engine_bridge
from adx.engine_bridge import PPQ, IdArray, Native, NoteArray, Project
from adx.panels.piano_roll import scales
from adx.panels.piano_roll.view_state import ViewState

#: Lane kinds, in engine/geometry/PianoRollGeometry.h's LaneKind order.
LANES: tuple[str, ...] = ("Velocity", "Pan", "Cutoff", "Resonance", "Fine pitch", "Release")


def lane_to_field(lane: int, value: float) -> tuple[str, float]:
    """The note field a lane edits, and the field value for a lane value in 0..1.

    The inverse of the engine's laneValue(); test_piano_roll_tools.py checks the two
    agree at the ends and the middle of every lane.
    """
    v = max(0.0, min(1.0, value))
    if lane == 0:
        return "velocity", float(max(1, round(v * 127)))
    if lane == 1:
        return "pan", v * 2.0 - 1.0
    if lane == 2:
        return "cutoff", v * 8.0 - 4.0
    if lane == 3:
        return "resonance", v
    if lane == 4:
        return "fine", float(round(v * 200.0 - 100.0))
    return "release", float(round(v * 127))


def field_to_lane(lane: int, note: np.void) -> float:
    """A note's value in a lane, 0..1: the engine's laneValue(), in Python."""
    if lane == 0:
        return float(note["velocity"]) / 127.0
    if lane == 1:
        return (float(note["pan"]) + 1.0) / 2.0
    if lane == 2:
        return (float(note["cutoff"]) + 4.0) / 8.0
    if lane == 3:
        return float(note["resonance"])
    if lane == 4:
        return (float(note["fine"]) + 100.0) / 200.0
    return float(note["release"]) / 127.0


def lane_values(lane: int, rows: NoteArray) -> npt.NDArray[np.float64]:
    """:func:`field_to_lane` over an array."""
    if lane == 0:
        return rows["velocity"].astype(np.float64) / 127.0
    if lane == 1:
        return (rows["pan"].astype(np.float64) + 1.0) / 2.0
    if lane == 2:
        return (rows["cutoff"].astype(np.float64) + 4.0) / 8.0
    if lane == 3:
        return rows["resonance"].astype(np.float64)
    if lane == 4:
        return (rows["fine"].astype(np.float64) + 100.0) / 200.0
    return rows["release"].astype(np.float64) / 127.0


def set_lane_values(lane: int, rows: NoteArray, values: npt.NDArray[np.float64]) -> NoteArray:
    """A copy of ``rows`` with the lane's field set from 0..1 values (lane_to_field)."""
    out = rows.copy()
    v = np.clip(values, 0.0, 1.0)
    if lane == 0:
        out["velocity"] = np.maximum(np.floor(v * 127 + 0.5), 1).astype(np.uint8)
    elif lane == 1:
        out["pan"] = v * 2.0 - 1.0
    elif lane == 2:
        out["cutoff"] = v * 8.0 - 4.0
    elif lane == 3:
        out["resonance"] = v
    elif lane == 4:
        out["fine"] = np.floor(v * 200.0 - 100.0 + 0.5).astype(np.int16)
    else:
        out["release"] = np.floor(v * 127 + 0.5).astype(np.uint16)
    return out


@dataclass(slots=True)
class RollModel:
    """One channel's notes in one pattern, and how the roll edits them."""

    project: Project
    pattern: str
    channel: str
    view: ViewState = field(default_factory=ViewState)
    grid: int = PPQ // 4
    snap: bool = True
    scale_root: int = 0
    scale_mask: int = 0
    lane: int = 0
    #: Length of a note drawn with a click and no drag.
    default_length: int = PPQ // 4
    #: Commands executed through commit(): one per gesture.
    commands: int = 0
    #: Called after every commit, so the panel rebuilds and the engine catches up.
    on_commit: Callable[[], None] | None = None
    _notes: NoteArray = field(default_factory=lambda: engine_bridge.empty_notes())
    _revision: int = -1
    _selection: IdArray = field(default_factory=lambda: np.zeros(0, dtype=np.uint32))
    _viewport: Native | None = None
    _viewport_key: tuple[float, ...] = ()

    # --- reading ------------------------------------------------------------------------

    def notes(self) -> NoteArray:
        """The clip's notes, re-read (one engine call) only when the project changed."""
        revision = self.project.revision()
        if revision != self._revision:
            self._notes = self.project.notes(self.pattern, self.channel)
            self._revision = revision
        return self._notes

    def invalidate(self) -> None:
        """Forget the cached notes (after an undo, a redo or a load)."""
        self._revision = -1

    def note(self, note_id: int) -> NoteArray:
        """The one-row array for a note, or an empty one."""
        notes = self.notes()
        row: NoteArray = notes[notes["id"] == note_id]
        return row

    def pattern_length(self) -> int:
        """The pattern's length in ticks."""
        return self.project.pattern_length(self.pattern)

    # --- hit testing (in C++: phase_5.md 4.6) -------------------------------------------

    def viewport(self) -> Native:
        """The visible note area as a native Viewport; rebuilt only when the view moved."""
        v = self.view
        key = (
            v.tick_start,
            v.pitch_top,
            v.pixels_per_tick,
            v.pixels_per_semitone,
            v.width,
            v.note_height,
        )
        if key != self._viewport_key:
            self._viewport = engine_bridge.make_viewport(*key)
            self._viewport_key = key
        return self._viewport

    def hit_note(self, x: float, y: float) -> tuple[int, int]:
        """(note id or 0, part) under a pixel of the note area."""
        if self.view.in_lane(y):
            return 0, 0
        return engine_bridge.hit_test_note(
            self.project, self.pattern, self.channel, self.viewport(), x, y
        )

    def hit_rect(self, rect: tuple[float, float, float, float]) -> IdArray:
        """Every note a pixel rectangle touches: one engine call, one array."""
        return engine_bridge.hit_test_rect(
            self.project, self.pattern, self.channel, self.viewport(), rect
        )

    def hit_lane(self, x: float, y: float) -> tuple[int, float]:
        """(note id or 0, lane value) under a pixel of the lane strip."""
        return engine_bridge.hit_test_lane(
            self.project,
            self.pattern,
            self.channel,
            self.viewport(),
            self.view.lane_height,
            x,
            y - self.view.note_height,
        )

    # --- selection ------------------------------------------------------------------

    @property
    def selection(self) -> IdArray:
        """Selected note ids, sorted and unique."""
        return self._selection

    def select(self, ids: IdArray | list[int], mode: str = "replace") -> None:
        """Change the selection: replace, add, toggle or remove."""
        chosen = np.unique(np.asarray(ids, dtype=np.uint32))
        if mode == "add":
            chosen = np.union1d(self._selection, chosen)
        elif mode == "toggle":
            chosen = np.setxor1d(self._selection, chosen)
        elif mode == "remove":
            chosen = np.setdiff1d(self._selection, chosen)
        existing = self.notes()["id"]
        self._selection = np.intersect1d(chosen, existing).astype(np.uint32)

    def selected_notes(self) -> NoteArray:
        """The selected rows."""
        notes = self.notes()
        return notes[np.isin(notes["id"], self._selection)]

    # --- snapping -------------------------------------------------------------------

    def snap_tick(self, tick: float) -> int:
        """Down to the grid (or to the tick, with snapping off)."""
        if not self.snap or self.grid <= 0:
            return max(0, round(tick))
        return max(0, int(tick // self.grid) * self.grid)

    def snap_tick_nearest(self, tick: float) -> int:
        """To the nearest grid line."""
        if not self.snap or self.grid <= 0:
            return max(0, round(tick))
        return max(0, round(tick / self.grid) * self.grid)

    def snap_pitch(self, pitch: int) -> int:
        """To the scale, if there is one."""
        return scales.snap_pitch(pitch, self.scale_root, self.scale_mask)

    def step(self) -> int:
        """The smallest length an edit makes: the grid, or one sixty-fourth."""
        return self.grid if self.snap and self.grid > 0 else PPQ // 16

    # --- editing --------------------------------------------------------------------

    def commit(self, command: Native) -> None:
        """Execute ONE command: the end of every gesture."""
        self.project.execute(command)
        self.commands += 1
        self.invalidate()
        if self.on_commit is not None:
            self.on_commit()

    def edit(
        self,
        label: str,
        remove: IdArray | None = None,
        update: NoteArray | None = None,
        add: NoteArray | None = None,
    ) -> None:
        """One EditNotes command: the shape of most gestures."""
        empty_ids = np.zeros(0, dtype=np.uint32)
        command = engine_bridge.commands.edit_notes(
            self.pattern,
            self.channel,
            empty_ids if remove is None else remove.astype(np.uint32),
            engine_bridge.empty_notes() if update is None else update,
            engine_bridge.empty_notes() if add is None else add,
            label,
        )
        self.commit(command)

    def new_notes(
        self, starts: list[int], lengths: list[int], pitches: list[int], velocity: int = 100
    ) -> NoteArray:
        """Rows for notes to add (ids are assigned by the engine)."""
        rows = engine_bridge.empty_notes(len(starts))
        rows["start"] = starts
        rows["length"] = [max(1, length) for length in lengths]
        rows["pitch"] = [max(0, min(127, p)) for p in pitches]
        rows["velocity"] = velocity
        rows["release"] = 64
        return rows

    def undo(self) -> bool:
        """Undo one step."""
        done = self.project.undo()
        self.invalidate()
        return done

    def redo(self) -> bool:
        """Redo one step."""
        done = self.project.redo()
        self.invalidate()
        return done
