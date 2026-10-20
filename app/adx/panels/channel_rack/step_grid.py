"""The channel rack's step grid: C++ cells, Python clicks (phase_6.md 4.2).

The grid edits the selected pattern's notes at step resolution - the same notes the
piano roll edits at full resolution, read fresh on every rebuild - so the two views
cannot disagree. A click on an empty step adds a note there (one EditNotes command);
a click on a lit step removes it; dragging a lit step up or down sets its velocity,
committed once on release.

Swing delays every second step by up to half a step as it is written, so a swung note
still belongs to its step (StepGridGeometry reads a note into the step its start is
nearest).
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

import numpy as np

from adx import engine_bridge
from adx.engine_bridge import PPQ, StepGridGeometry
from adx.panels.channel_rack.channel_row import ROW_HEIGHT
from adx.panels.geometry_view import LEFT, RIGHT, WORLD_TRIANGLES, GeometryView, Pointer
from adx.session import Session

#: The pitch a step writes: C5.
STEP_PITCH = 60
STEP_VELOCITY = 100
#: Pixels of drag for the full velocity range.
VELOCITY_PIXELS = 120.0


@dataclass(slots=True)
class GridState:
    """What the grid shows: view state, never project data."""

    pattern_id: int = 0
    pattern: str = ""
    channels: tuple[tuple[int, str], ...] = ()
    steps: int = 16
    step_ticks: int = PPQ // 4
    swing: float = 0.0
    cell_width: float = 22.0


class StepGrid:
    """The grid view and what a click on it means."""

    def __init__(self, session: Session, audition: Callable[[int, int], None]) -> None:
        self.session = session
        self.audition = audition
        self.state = GridState()
        self.builder = StepGridGeometry()
        self.view = GeometryView(self, (WORLD_TRIANGLES, WORLD_TRIANGLES))
        self.commands = 0
        self._drag: tuple[int, int, float, int] | None = None
        self._moved = False

    # --- geometry -------------------------------------------------------------------

    def rebuild(self) -> None:
        """Build every cell in C++ and upload: one build, one lease per layer."""
        state = self.state
        project = self.session.project
        width = state.steps * state.cell_width
        height = len(state.channels) * ROW_HEIGHT
        self.view.setFixedSize(int(max(width, 1)), int(max(height, 1)))
        if project is None or not state.pattern or not state.channels:
            self.view.clear()
            return
        ids = np.asarray([c for c, _n in state.channels], dtype=np.uint32)
        self.builder.build(project, state.pattern_id, ids, state.step_ticks, state.steps, 4)
        for layer in (0, 1):
            with self.builder.view(layer) as lease:
                self.view.upload(layer, lease)
        self.view.set_view(0.0, 0.0, state.cell_width, float(ROW_HEIGHT))

    # --- input ----------------------------------------------------------------------

    def cell_at(self, x: float, y: float) -> tuple[int, int] | None:
        """(row, step) under a pixel, or None."""
        row, step = int(y // ROW_HEIGHT), int(x // self.state.cell_width)
        if 0 <= row < len(self.state.channels) and 0 <= step < self.state.steps:
            return row, step
        return None

    def step_tick(self, step: int) -> int:
        """Where a step's note starts: on the grid, every second step swung late."""
        tick = step * self.state.step_ticks
        if step % 2 == 1:
            tick += round(self.state.swing * self.state.step_ticks * 0.5)
        return tick

    def on_press(self, pointer: Pointer) -> None:
        cell = self.cell_at(pointer.x, pointer.y)
        project = self.session.project
        if cell is None or project is None:
            return
        row, step = cell
        channel, _name = self.state.channels[row]
        steps = engine_bridge.steps_of(
            project, self.state.pattern_id, channel, self.state.step_ticks, self.state.steps
        )
        note, velocity = int(steps[step][0]), int(steps[step][1])
        if note and pointer.button == RIGHT:
            self._remove(row, note)
        elif note:
            self._drag = (row, note, pointer.y, velocity)
            self._moved = False
        elif pointer.button == LEFT:
            self._add(row, step)

    def on_move(self, pointer: Pointer) -> None:
        if self._drag is None:
            return
        _row, _note, y0, velocity = self._drag
        if abs(pointer.y - y0) > 2:
            self._moved = True
        new = self._velocity_for(velocity, y0 - pointer.y)
        self.view.set_hint(f"velocity {new}")

    def on_release(self, pointer: Pointer) -> None:
        if self._drag is None:
            return
        row, note, y0, velocity = self._drag
        self._drag = None
        self.view.set_hint("")
        if self._moved:
            self._set_velocity(row, note, self._velocity_for(velocity, y0 - pointer.y))
        else:
            self._remove(row, note)

    def on_double_click(self, pointer: Pointer) -> None:
        pass

    def on_wheel(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None:
        pass

    def on_resize(self, width: float, height: float) -> None:
        pass

    @staticmethod
    def _velocity_for(start: int, pixels: float) -> int:
        return max(1, min(127, round(start + pixels / VELOCITY_PIXELS * 127)))

    # --- the edits: one command each ------------------------------------------------

    def _edit(
        self,
        row: int,
        label: str,
        remove: list[int],
        update: np.ndarray | None,
        add: np.ndarray | None,
    ) -> None:
        _channel, name = self.state.channels[row]
        command = engine_bridge.commands.edit_notes(
            self.state.pattern,
            name,
            np.asarray(remove, dtype=np.uint32),
            engine_bridge.empty_notes() if update is None else update,
            engine_bridge.empty_notes() if add is None else add,
            label,
        )
        if self.session.execute(command):
            self.commands += 1

    def _add(self, row: int, step: int) -> None:
        notes = engine_bridge.empty_notes(1)
        notes["start"] = self.step_tick(step)
        notes["length"] = self.state.step_ticks
        notes["pitch"] = STEP_PITCH
        notes["velocity"] = STEP_VELOCITY
        notes["release"] = 64
        self._edit(row, "Add step", [], None, notes)
        self.audition(self.state.channels[row][0], STEP_PITCH)

    def _remove(self, row: int, note: int) -> None:
        self._edit(row, "Remove step", [note], None, None)

    def _set_velocity(self, row: int, note: int, velocity: int) -> None:
        project = self.session.require()
        _channel, name = self.state.channels[row]
        notes = project.notes(self.state.pattern, name)
        chosen = notes[notes["id"] == note].copy()
        if len(chosen) == 0:
            return
        chosen["velocity"] = velocity
        self._edit(row, "Step velocity", [], chosen, None)
