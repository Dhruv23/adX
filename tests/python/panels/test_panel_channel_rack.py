"""The channel rack (phase_6.md 4.2, 5)."""

from __future__ import annotations

import numpy as np
from PySide6.QtWidgets import QWidget

import ui_input
from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow
from adx.panels.channel_rack.channel_row import ROW_HEIGHT
from adx.panels.channel_rack.step_grid import STEP_PITCH, STEP_VELOCITY, StepGrid
from adx.panels.geometry_view import RIGHT, Pointer


def click(grid: StepGrid, row: int, step: int, button: int | None = None) -> None:
    """A click on one cell, through the grid view's input."""
    x = (step + 0.5) * grid.state.cell_width
    y = (row + 0.5) * ROW_HEIGHT
    ui_input.click(grid.view, Pointer(x, y) if button is None else Pointer(x, y, button))


def test_channel_rack_step_roundtrip(studio: MainWindow) -> None:
    rack = studio.channel_rack
    rack.select_pattern("P1")
    grid = rack.grid
    assert grid.state.pattern == "P1"
    # Step 4 of the Lead row: one command, one note at the step's tick.
    before = studio.project.undo_depth()
    click(grid, 0, 4)
    assert studio.project.undo_depth() == before + 1
    notes = studio.project.notes("P1", "Lead")
    assert len(notes) == 1
    assert int(notes[0]["start"]) == 4 * (PPQ // 4)
    assert int(notes[0]["pitch"]) == STEP_PITCH
    # The piano roll is the same data at full resolution, read live.
    studio.piano_roll.select_clip("P1", "Lead")
    roll = studio.piano_roll.model.notes()
    assert [(int(n["start"]), int(n["velocity"])) for n in roll] == [
        (4 * (PPQ // 4), STEP_VELOCITY)
    ]
    # A velocity drag on the lit step: one command, the roll sees it.
    x = 4.5 * grid.state.cell_width
    y = 0.5 * ROW_HEIGHT
    ui_input.drag(grid.view, Pointer(x, y), Pointer(x, y + 30))
    velocity = int(studio.project.notes("P1", "Lead")[0]["velocity"])
    assert velocity < STEP_VELOCITY
    assert int(studio.piano_roll.model.notes()[0]["velocity"]) == velocity
    # And an edit in the roll's data shows in the grid: the step reads lit.
    from adx import engine_bridge

    steps = engine_bridge.steps_of(studio.project, grid.state.pattern_id, 1, PPQ // 4, 16)
    assert int(steps[4][0]) != 0
    # Clicking the lit step again removes it.
    click(grid, 0, 4)
    assert len(studio.project.notes("P1", "Lead")) == 0


def test_channel_rack_grid_is_cpp(studio: MainWindow) -> None:
    rack = studio.channel_rack
    for _ in range(63):
        studio.session.build_and_execute(
            "AddChannelNew", f"C{len(studio.project.channels_info())}", "additive", 0, ""
        )
    rack.steps_box.setCurrentIndex(2)  # 64 steps
    rack.select_pattern("P1")
    assert len(rack.grid.state.channels) == 64
    assert rack.grid.state.steps == 64
    # 64 x 64 cells and not one per-cell Python widget: the grid is one view.
    widgets = rack.grid.view.findChildren(QWidget)
    assert len(widgets) < 10
    # The geometry exists - every cell drawn by the C++ builder.
    with rack.grid.builder.view(0) as lease:
        assert lease.vertex_count >= 64 * 64 * 6


def test_add_channel_and_right_click_remove(studio: MainWindow) -> None:
    rack = studio.channel_rack
    rack.instrument_box.setCurrentText("fm")
    rack.add_channel()
    infos = studio.project.channels_info()
    assert [str(c["instrument"]) for c in infos] == ["additive", "fm"]
    rack.select_pattern("P1")
    click(rack.grid, 1, 0)
    assert len(studio.project.notes("P1", "Channel2")) == 1
    click(rack.grid, 1, 0, RIGHT)
    assert len(studio.project.notes("P1", "Channel2")) == 0
    # The rows follow the engine: undo removes nothing the rack keeps.
    while studio.session.undo():
        pass
    rack.refresh()
    assert len(rack.rows) == 1
    assert np.all(studio.project.notes("P1", "Lead")["id"] >= 0)
