"""The piano roll's editing tools (phase_5.md 4.7).

Each tool is a small state machine over press, move and release, and **every gesture
ends in exactly one command** - which is what makes them all undoable without any
per-tool undo code. A click that changes nothing (a selection, a drag back to where it
started) issues none.

The tools never loop over notes calling the engine. They read the clip as one array
(``RollModel.notes``), ask the engine's hit tests which note is under the pointer, and
compute an edit with numpy.

Positions are the scene-graph item's pixels; buttons, modifiers and keys are Qt's
integer values, so the tools are plain Python and test without a window.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

import numpy as np

from adx import engine_bridge
from adx.engine_bridge import PART_LEFT_EDGE, PART_RIGHT_EDGE, NoteArray
from adx.panels.piano_roll.model import LANES, RollModel, lane_values, set_lane_values

LEFT, RIGHT, MIDDLE = 0x1, 0x2, 0x4
SHIFT, CTRL, ALT = 0x02000000, 0x04000000, 0x08000000
KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN = 0x01000012, 0x01000013, 0x01000014, 0x01000015
KEY_DELETE, KEY_BACKSPACE = 0x01000007, 0x01000003

#: Curve kinds a slide cycles through (engine/core/Curve.h: Linear, Exponential,
#: Logarithmic, ... Smooth).
SLIDE_CURVES = (0, 1, 2, 4)


@dataclass(frozen=True, slots=True)
class Pointer:
    """One pointer event in item pixels."""

    x: float
    y: float
    button: int = LEFT
    modifiers: int = 0

    def has(self, modifier: int) -> bool:
        """Whether a modifier key is held."""
        return bool(self.modifiers & modifier)


#: A note drawn while a drag is in flight: (tick start, tick end, pitch).
PreviewNote = tuple[float, float, int]


@dataclass(slots=True)
class Tool:
    """The base: no-op handlers, a preview and an optional rubber band."""

    name: str = "tool"
    label: str = "Tool"
    preview: list[PreviewNote] = field(default_factory=list)
    band: tuple[float, float, float, float] | None = None

    def press(self, model: RollModel, p: Pointer) -> None:
        """A button went down."""

    def move(self, model: RollModel, p: Pointer) -> None:
        """The pointer moved with a button down."""

    def release(self, model: RollModel, p: Pointer) -> None:
        """The button came up: commit the gesture."""

    def key(self, model: RollModel, key: int, modifiers: int) -> bool:
        """A key press; True when the tool used it."""
        return False

    def reset(self) -> None:
        """Forget any gesture in flight."""
        self.preview = []
        self.band = None


def _rows_for(model: RollModel, ids: np.ndarray) -> NoteArray:
    notes = model.notes()
    return notes[np.isin(notes["id"], ids)].copy()


def _targets(model: RollModel, hit: int) -> np.ndarray:
    """The notes a gesture on ``hit`` acts on: the selection if hit is in it."""
    if hit in model.selection:
        return model.selection.copy()
    return np.array([hit], dtype=np.uint32)


def _preview_of(rows: NoteArray) -> list[PreviewNote]:
    # Bounded: dragging 10,000 selected notes previews the first 256 (the rest move with
    # them on release). The preview is drawn by QML, not built as geometry.
    return [
        (float(r["start"]), float(r["start"] + r["length"]), int(r["pitch"])) for r in rows[:256]
    ]


def _delete(model: RollModel, ids: np.ndarray) -> None:
    model.select(ids, "remove")
    model.edit("Delete notes", remove=ids)


@dataclass(slots=True)
class _Drag:
    """A move or resize in flight."""

    mode: str = ""
    ids: np.ndarray = field(default_factory=lambda: np.zeros(0, dtype=np.uint32))
    rows: NoteArray = field(default_factory=lambda: engine_bridge.empty_notes())
    origin_tick: float = 0.0
    origin_row: int = 0
    delta_ticks: int = 0
    delta_rows: int = 0

    def begin(self, model: RollModel, mode: str, ids: np.ndarray, p: Pointer) -> None:
        self.mode = mode
        self.ids = ids
        self.rows = _rows_for(model, ids)
        self.origin_tick = model.view.tick_at(p.x)
        self.origin_row = model.view.row_at(p.y)
        self.delta_ticks = 0
        self.delta_rows = 0

    def track(self, model: RollModel, p: Pointer) -> NoteArray:
        raw = model.view.tick_at(p.x) - self.origin_tick
        step = model.grid if model.snap and model.grid > 0 else 1
        self.delta_ticks = round(raw / step) * step
        self.delta_rows = model.view.row_at(p.y) - self.origin_row if self.mode == "move" else 0
        return self.result(model)

    def result(self, model: RollModel) -> NoteArray:
        out = self.rows.copy()
        starts = out["start"].astype(np.int64)
        lengths = out["length"].astype(np.int64)
        minimum = model.step()
        if self.mode == "move":
            starts = np.maximum(starts + self.delta_ticks, 0)
            pitches = np.clip(out["pitch"].astype(np.int64) + self.delta_rows, 0, 127)
            if model.scale_mask and self.delta_rows:
                pitches = np.array([model.snap_pitch(int(q)) for q in pitches], dtype=np.int64)
            out["pitch"] = pitches.astype(np.uint8)
        elif self.mode == "right":
            lengths = np.maximum(lengths + self.delta_ticks, minimum)
        elif self.mode == "left":
            ends = starts + lengths
            starts = np.clip(starts + self.delta_ticks, 0, ends - minimum)
            lengths = ends - starts
        out["start"] = starts
        out["length"] = lengths
        return out

    def changed(self) -> bool:
        return self.delta_ticks != 0 or self.delta_rows != 0


@dataclass(slots=True)
class DrawTool(Tool):
    """Click-drag draws a note; drag a body to move, an edge to resize; right-click deletes."""

    name: str = "draw"
    label: str = "Draw"
    _drag: _Drag = field(default_factory=_Drag)
    _create: tuple[int, int] | None = None
    velocity: int = 100

    def press(self, model: RollModel, p: Pointer) -> None:
        self.reset()
        hit, part = model.hit_note(p.x, p.y)
        if p.button == RIGHT:
            if hit:
                _delete(model, _targets(model, hit))
            return
        if hit:
            if hit not in model.selection:
                model.select([hit])
            mode = {PART_RIGHT_EDGE: "right", PART_LEFT_EDGE: "left"}.get(part, "move")
            self._drag.begin(model, mode, _targets(model, hit), p)
            self.velocity = (
                int(self._drag.rows["velocity"][0]) if len(self._drag.rows) else self.velocity
            )
            return
        start = model.snap_tick(model.view.tick_at(p.x))
        pitch = model.snap_pitch(model.view.row_at(p.y))
        self._create = (start, pitch)
        self.preview = [(start, start + model.default_length, pitch)]

    def move(self, model: RollModel, p: Pointer) -> None:
        if self._create is not None:
            start, pitch = self._create
            end = max(start + model.step(), model.snap_tick_nearest(model.view.tick_at(p.x)))
            self.preview = [(start, end, pitch)]
        elif self._drag.mode:
            self.preview = _preview_of(self._drag.track(model, p))

    def release(self, model: RollModel, p: Pointer) -> None:
        if self._create is not None:
            start, pitch = self._create
            end = self.preview[0][1] if self.preview else start + model.default_length
            length = int(end - start)
            model.default_length = length
            model.edit("Draw note", add=model.new_notes([start], [length], [pitch], self.velocity))
            created = model.notes()
            match = created[(created["start"] == start) & (created["pitch"] == pitch)]
            if len(match):
                model.select([int(match["id"].max())])
        elif self._drag.mode:
            self._drag.track(model, p)
            if self._drag.changed():
                label = "Move notes" if self._drag.mode == "move" else "Resize notes"
                model.edit(label, update=self._drag.result(model))
        self._create = None
        self._drag = _Drag()
        self.reset()


@dataclass(slots=True)
class PaintTool(Tool):
    """Drag paints a run of notes at the grid division; right-drag erases."""

    name: str = "paint"
    label: str = "Paint"
    _cells: dict[int, int] = field(default_factory=dict)
    _erase: set[int] = field(default_factory=set)
    _last: int | None = None
    _painting: bool = False

    def press(self, model: RollModel, p: Pointer) -> None:
        self.reset()
        self._cells = {}
        self._erase = set()
        self._painting = p.button == LEFT
        self._last = None
        self.move(model, p)

    def move(self, model: RollModel, p: Pointer) -> None:
        if not self._painting:
            hit, _part = model.hit_note(p.x, p.y)
            if hit:
                self._erase.add(hit)
            return
        step = model.step()
        cell = model.snap_tick(model.view.tick_at(p.x))
        pitch = model.snap_pitch(model.view.row_at(p.y))
        first = cell if self._last is None else min(cell, self._last + step)
        for start in range(first, cell + 1, step):
            self._cells.setdefault(start, pitch)
        self._last = cell
        self.preview = [(s, s + step, q) for s, q in sorted(self._cells.items())]

    def release(self, model: RollModel, p: Pointer) -> None:
        if self._painting and self._cells:
            notes = model.notes()
            existing = notes["start"].astype(np.int64) * 128 + notes["pitch"].astype(np.int64)
            wanted = sorted(self._cells.items())
            keys = np.array([s * 128 + q for s, q in wanted], dtype=np.int64)
            fresh = ~np.isin(keys, existing)
            cells = [cell for cell, keep in zip(wanted, fresh, strict=True) if keep]
            if cells:
                step = model.step()
                model.edit(
                    "Paint notes",
                    add=model.new_notes(
                        [s for s, _ in cells], [step] * len(cells), [q for _, q in cells]
                    ),
                )
        elif self._erase:
            _delete(model, np.array(sorted(self._erase), dtype=np.uint32))
        self._painting = False
        self.reset()


@dataclass(slots=True)
class SelectTool(Tool):
    """Rubber band, shift adds, ctrl toggles; drag a selected note to move; arrows nudge."""

    name: str = "select"
    label: str = "Select"
    _drag: _Drag = field(default_factory=_Drag)
    _origin: tuple[float, float] | None = None
    _mode: str = "replace"

    def press(self, model: RollModel, p: Pointer) -> None:
        self.reset()
        self._mode = "add" if p.has(SHIFT) else "toggle" if p.has(CTRL) else "replace"
        hit, _part = model.hit_note(p.x, p.y)
        if hit and p.button == LEFT:
            if self._mode == "toggle" or hit not in model.selection:
                model.select([hit], self._mode)
            if hit in model.selection:
                self._drag.begin(model, "move", model.selection.copy(), p)
            return
        self._origin = (p.x, p.y)
        self.band = (p.x, p.y, p.x, p.y)

    def move(self, model: RollModel, p: Pointer) -> None:
        if self._origin is not None:
            self.band = (self._origin[0], self._origin[1], p.x, p.y)
        elif self._drag.mode:
            self.preview = _preview_of(self._drag.track(model, p))

    def release(self, model: RollModel, p: Pointer) -> None:
        if self._origin is not None:
            x0, y0 = self._origin
            ids = model.hit_rect((x0, y0, p.x, p.y))
            model.select(ids, self._mode)
        elif self._drag.mode:
            self._drag.track(model, p)
            if self._drag.changed():
                model.edit("Move notes", update=self._drag.result(model))
        self._origin = None
        self._drag = _Drag()
        self.reset()

    def key(self, model: RollModel, key: int, modifiers: int) -> bool:
        if key in (KEY_DELETE, KEY_BACKSPACE):
            if len(model.selection):
                _delete(model, model.selection.copy())
            return True
        moves = {KEY_LEFT: (-1, 0), KEY_RIGHT: (1, 0), KEY_UP: (0, 1), KEY_DOWN: (0, -1)}
        if key not in moves or not len(model.selection):
            return key in moves
        dt, dp = moves[key]
        rows = model.selected_notes().copy()
        shift = bool(modifiers & SHIFT)
        if dt:
            rows["start"] = np.maximum(rows["start"].astype(np.int64) + dt * model.step(), 0)
        if dp:
            rows["pitch"] = np.clip(
                rows["pitch"].astype(np.int64) + dp * (12 if shift else 1), 0, 127
            ).astype(np.uint8)
        model.edit("Nudge notes", update=rows)
        return True


@dataclass(slots=True)
class SliceTool(Tool):
    """Click splits a note at the cursor tick (every selected note, if it is selected)."""

    name: str = "slice"
    label: str = "Slice"

    def release(self, model: RollModel, p: Pointer) -> None:
        hit, _part = model.hit_note(p.x, p.y)
        if not hit:
            return
        tick = model.snap_tick_nearest(model.view.tick_at(p.x))
        rows = _rows_for(model, _targets(model, hit))
        starts = rows["start"].astype(np.int64)
        ends = starts + rows["length"].astype(np.int64)
        crossing = (starts < tick) & (ends > tick)
        if not crossing.any():
            return
        left = rows[crossing].copy()
        right = rows[crossing].copy()
        left["length"] = tick - left["start"].astype(np.int64)
        right["length"] = ends[crossing] - tick
        right["start"] = tick
        model.edit("Slice notes", update=left, add=right)


def glue_rows(rows: NoteArray, tolerance: int) -> tuple[NoteArray, np.ndarray]:
    """Merge touching notes on each pitch. Returns (lengthened rows, ids to remove)."""
    order = np.lexsort((rows["start"], rows["pitch"]))
    keep: list[Any] = []
    remove: list[int] = []
    current: Any = None
    end = 0
    for row in rows[order]:
        start = int(row["start"])
        if current is not None and row["pitch"] == current["pitch"] and start <= end + tolerance:
            end = max(end, start + int(row["length"]))
            remove.append(int(row["id"]))
            continue
        if current is not None:
            current["length"] = end - int(current["start"])
            keep.append(current)
        current = row.copy()
        end = start + int(row["length"])
    if current is not None:
        current["length"] = end - int(current["start"])
        keep.append(current)
    merged = np.array(keep, dtype=rows.dtype) if keep else rows[:0]
    changed = merged[np.isin(merged["id"], rows["id"])]
    return changed, np.array(remove, dtype=np.uint32)


@dataclass(slots=True)
class GlueTool(Tool):
    """Merges adjacent selected notes on the same pitch (or the clicked note's run)."""

    name: str = "glue"
    label: str = "Glue"

    def release(self, model: RollModel, p: Pointer) -> None:
        hit, _part = model.hit_note(p.x, p.y)
        if len(model.selection) >= 2 and (not hit or hit in model.selection):
            rows = model.selected_notes()
        elif hit:
            notes = model.notes()
            row = notes[notes["id"] == hit]
            rows = notes[notes["pitch"] == row["pitch"][0]]
        else:
            return
        lengthened, remove = glue_rows(rows, tolerance=0)
        if len(remove):
            model.select(remove, "remove")
            model.edit("Glue notes", update=lengthened, remove=remove)


@dataclass(slots=True)
class StrumTool(Tool):
    """Drag across a chord to spread its onsets: right for up, left for down, shift curves."""

    name: str = "strum"
    label: str = "Strum"
    _chord: NoteArray = field(default_factory=lambda: engine_bridge.empty_notes())
    _origin: float = 0.0

    def press(self, model: RollModel, p: Pointer) -> None:
        self.reset()
        tick = model.view.tick_at(p.x)
        if len(model.selection) >= 2:
            chord = model.selected_notes()
        else:
            notes = model.notes()
            starts = notes["start"].astype(np.int64)
            chord = notes[(starts <= tick) & (starts + notes["length"].astype(np.int64) > tick)]
        self._chord = chord[np.argsort(chord["pitch"], kind="stable")].copy()
        self._origin = tick

    def strummed(self, model: RollModel, span: float, curved: bool) -> NoteArray:
        """The chord with onsets spread over ``span`` ticks (negative: downward)."""
        rows = self._chord.copy()
        n = len(rows)
        if n < 2:
            return rows
        fractions = np.linspace(0.0, 1.0, n)
        if curved:
            fractions = fractions**2
        if span < 0:
            fractions = fractions[::-1]
        starts = rows["start"].astype(np.int64)
        ends = starts + rows["length"].astype(np.int64)
        new_starts = np.minimum(
            starts + np.floor(fractions * abs(span) + 0.5).astype(np.int64), ends - 1
        )
        rows["start"] = new_starts
        rows["length"] = ends - new_starts
        return rows

    def move(self, model: RollModel, p: Pointer) -> None:
        if len(self._chord) >= 2:
            span = model.view.tick_at(p.x) - self._origin
            self.preview = _preview_of(self.strummed(model, span, p.has(SHIFT)))

    def release(self, model: RollModel, p: Pointer) -> None:
        span = model.view.tick_at(p.x) - self._origin
        if len(self._chord) >= 2 and abs(span) >= 1.0:
            model.edit("Strum", update=self.strummed(model, span, p.has(SHIFT)))
        self._chord = engine_bridge.empty_notes()
        self.reset()


@dataclass(slots=True)
class MuteTool(Tool):
    """Click toggles mute (the whole selection, if the note is selected); drag sweeps."""

    name: str = "mute"
    label: str = "Mute"
    _ids: set[int] = field(default_factory=set)
    _target: bool = True

    def press(self, model: RollModel, p: Pointer) -> None:
        self._ids = set()
        hit, _part = model.hit_note(p.x, p.y)
        if not hit:
            return
        self._target = not bool(model.note(hit)["muted"][0])
        self._ids.update(int(i) for i in _targets(model, hit))

    def move(self, model: RollModel, p: Pointer) -> None:
        if self._ids:
            hit, _part = model.hit_note(p.x, p.y)
            if hit:
                self._ids.add(hit)

    def release(self, model: RollModel, p: Pointer) -> None:
        if self._ids:
            rows = _rows_for(model, np.array(sorted(self._ids), dtype=np.uint32))
            rows = rows[rows["muted"] != int(self._target)]
            if len(rows):
                rows["muted"] = int(self._target)
                model.edit("Mute notes" if self._target else "Unmute notes", update=rows)
        self._ids = set()


@dataclass(slots=True)
class SlideTool(Tool):
    """Drag from a note to another pitch to set its slide; alt targets the next note's
    head; shift-click cycles the curve; right-click clears."""

    name: str = "slide"
    label: str = "Slide"
    _note: int = 0
    _press_tick: int = 0

    def press(self, model: RollModel, p: Pointer) -> None:
        self.reset()
        hit, _part = model.hit_note(p.x, p.y)
        self._note = hit
        if not hit:
            return
        row = model.note(hit)
        if p.button == RIGHT:
            model.commit(
                engine_bridge.commands.set_slide(model.pattern, model.channel, hit, None, 0, 1, 0)
            )
            self._note = 0
            return
        start = int(row["start"][0])
        self._press_tick = min(
            max(model.snap_tick(model.view.tick_at(p.x)), start), start + int(row["length"][0]) - 1
        )

    def move(self, model: RollModel, p: Pointer) -> None:
        if self._note:
            row = model.note(self._note)
            self.preview = [
                (
                    self._press_tick,
                    model.view.tick_at(p.x),
                    model.snap_pitch(model.view.row_at(p.y)),
                )
            ]
            if not len(row):
                self.preview = []

    def release(self, model: RollModel, p: Pointer) -> None:
        note = self._note
        self._note = 0
        self.reset()
        if not note:
            return
        row = model.note(note)
        start = int(row["start"][0])
        length = int(row["length"][0])
        pitch = int(row["pitch"][0])
        rel_start = self._press_tick - start
        end_tick = model.snap_tick_nearest(model.view.tick_at(p.x))
        if p.has(SHIFT) and abs(end_tick - self._press_tick) < model.step():
            self._cycle_curve(model, note)
            return
        target = model.snap_pitch(model.view.row_at(p.y))
        if p.has(ALT):
            head, _part = model.hit_note(p.x, p.y)
            if head and head != note:
                target = int(model.note(head)["pitch"][0])
        cents = (target - pitch) * 100
        glide = max(1, min(length - rel_start, max(model.step(), end_tick - self._press_tick)))
        model.commit(
            engine_bridge.commands.set_slide(
                model.pattern, model.channel, note, cents if cents else None, rel_start, glide, 0
            )
        )

    def _cycle_curve(self, model: RollModel, note: int) -> None:
        slide = model.project.note_extras(model.pattern, model.channel, note)["slide"]
        if slide is None:
            return
        cents, start, length, curve, _tension = slide
        following = (
            SLIDE_CURVES[(SLIDE_CURVES.index(curve) + 1) % len(SLIDE_CURVES)]
            if curve in SLIDE_CURVES
            else 0
        )
        model.commit(
            engine_bridge.commands.set_slide(
                model.pattern, model.channel, note, int(cents), int(start), int(length), following
            )
        )


def simplify(points: list[tuple[int, int]], tolerance: float) -> list[tuple[int, int]]:
    """Ramer-Douglas-Peucker over (tick, cents): drop points within ``tolerance`` cents."""
    if len(points) <= 2:
        return list(points)
    (x0, y0), (x1, y1) = points[0], points[-1]
    worst, index = 0.0, 0
    for i in range(1, len(points) - 1):
        x, y = points[i]
        expected = y0 if x1 == x0 else y0 + (y1 - y0) * (x - x0) / (x1 - x0)
        distance = abs(y - expected)
        if distance > worst:
            worst, index = distance, i
    if worst <= tolerance:
        return [points[0], points[-1]]
    return simplify(points[: index + 1], tolerance)[:-1] + simplify(points[index:], tolerance)


@dataclass(slots=True)
class PitchCurveTool(Tool):
    """Pencil a per-note pitch curve over a note; right-click clears it."""

    name: str = "bend"
    label: str = "Pitch curve"
    tolerance: float = 4.0
    _note: int = 0
    _start: int = 0
    _pitch: int = 0
    _points: list[tuple[int, int]] = field(default_factory=list)

    def press(self, model: RollModel, p: Pointer) -> None:
        self.reset()
        hit, _part = model.hit_note(p.x, p.y)
        self._note = hit
        self._points = []
        if not hit:
            return
        if p.button == RIGHT:
            empty = np.zeros((0, 2), dtype=np.int64)
            model.commit(
                engine_bridge.commands.set_pitch_curve(model.pattern, model.channel, hit, empty)
            )
            self._note = 0
            return
        row = model.note(hit)
        self._start = int(row["start"][0])
        self._pitch = int(row["pitch"][0])
        self.move(model, p)

    def move(self, model: RollModel, p: Pointer) -> None:
        if not self._note:
            return
        at = max(0, round(model.view.tick_at(p.x)) - self._start)
        cents = round((model.view.pitch_value_at(p.y) - (self._pitch + 0.5)) * 100)
        if not self._points or at > self._points[-1][0]:
            self._points.append((at, cents))
        self.preview = [
            (self._start + a, self._start + a + 1, self._pitch + round(c / 100))
            for a, c in self._points[-64:]
        ]

    def release(self, model: RollModel, p: Pointer) -> None:
        note = self._note
        self._note = 0
        self.reset()
        if not note or not self._points:
            return
        points = simplify(self._points, self.tolerance)
        model.commit(
            engine_bridge.commands.set_pitch_curve(
                model.pattern, model.channel, note, np.array(points, dtype=np.int64).reshape(-1, 2)
            )
        )


def simplify_curve(model: RollModel, note: int, tolerance: float = 10.0) -> bool:
    """The "simplify" action: thin one note's drawn curve. One command, or none."""
    bend = model.project.note_extras(model.pattern, model.channel, note)["bend"]
    points = [(int(at), int(cents)) for at, cents, _curve in bend]
    thinned = simplify(points, tolerance)
    if len(thinned) == len(points):
        return False
    model.commit(
        engine_bridge.commands.set_pitch_curve(
            model.pattern, model.channel, note, np.array(thinned, dtype=np.int64).reshape(-1, 2)
        )
    )
    return True


@dataclass(slots=True)
class LaneEditor:
    """Dragging a bar in the lane strip; with several selected, scales them together."""

    rows: NoteArray = field(default_factory=lambda: engine_bridge.empty_notes())
    grabbed: float = 0.0
    value: float = 0.0
    active: bool = False

    def press(self, model: RollModel, p: Pointer) -> bool:
        """Start a lane drag; False when no bar is under the pointer."""
        note, value = model.hit_lane(p.x, p.y)
        if not note:
            return False
        self.rows = _rows_for(model, _targets(model, note))
        grabbed = lane_values(model.lane, self.rows[self.rows["id"] == note])
        self.grabbed = float(grabbed[0]) if len(grabbed) else value
        self.value = value
        self.active = True
        return True

    def edited(self, model: RollModel) -> NoteArray:
        """The rows with the lane set: the grabbed bar to the pointer, the rest scaled."""
        current = lane_values(model.lane, self.rows)
        if len(self.rows) == 1:
            target = np.full(1, self.value)
        elif model.lane in (1, 2, 4):  # bipolar: scale the distance from the middle
            base = self.grabbed - 0.5
            factor = (self.value - 0.5) / base if abs(base) > 1e-6 else 1.0
            target = 0.5 + (current - 0.5) * factor
        else:
            factor = self.value / self.grabbed if self.grabbed > 1e-6 else 1.0
            target = current * factor
        return set_lane_values(model.lane, self.rows, target)

    def move(self, model: RollModel, p: Pointer) -> None:
        """Track the pointer."""
        lane_top = model.view.note_height
        self.value = max(0.0, min(1.0, 1.0 - (p.y - lane_top) / max(1.0, model.view.lane_height)))

    def release(self, model: RollModel, p: Pointer) -> None:
        """Commit: one EditNotes."""
        if self.active:
            self.move(model, p)
            model.edit(f"Edit {LANES[model.lane].lower()}", update=self.edited(model))
        self.active = False


#: The seven tools phase_5.md 5's table drives, then the two extension tools.
TOOLS: tuple[type[Tool], ...] = (
    DrawTool,
    PaintTool,
    SelectTool,
    SliceTool,
    GlueTool,
    StrumTool,
    MuteTool,
    SlideTool,
    PitchCurveTool,
)
