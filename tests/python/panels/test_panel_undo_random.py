"""Undo works from every panel (phase_6.md 6): a seeded random walk of UI operations
across the rack, playlist, mixer, editors, automation and the selection, then undo to
empty - the project must be byte-identical to where it started - and redo to the end,
byte-identical to where it finished. In the spirit of Phase 2's test_undo_random."""

from __future__ import annotations

import pathlib
import random
from collections.abc import Callable

import numpy as np
from PySide6.QtWidgets import QWidget

import ui_input
from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow
from adx.panels.channel_rack.channel_row import ROW_HEIGHT
from adx.panels.geometry_view import Pointer
from adx.panels.params import GenericEditor
from adx.selection import Context, Kind
from ui_input import point, ready

STEPS = 80


def operations(studio: MainWindow, rng: random.Random) -> list[Callable[[], None]]:
    """Every operation the walk may take; each picks its own target."""
    project, session = studio.project, studio.session

    def rack_add_channel() -> None:
        studio.channel_rack.instrument_box.setCurrentIndex(rng.randrange(5))
        studio.channel_rack.add_channel()

    def rack_step() -> None:
        rack = studio.channel_rack
        rack.select_pattern("P1")
        rows = len(rack.grid.state.channels)
        x = (rng.randrange(16) + 0.5) * rack.grid.state.cell_width
        ui_input.click(rack.grid.view, Pointer(x, (rng.randrange(rows) + 0.5) * ROW_HEIGHT))

    def playlist_place() -> None:
        playlist = studio.playlist
        playlist.set_tool("draw")
        playlist.toolbar.choose_pattern(int(project.patterns_info()[0]["id"]))
        ui_input.click(playlist.canvas, point(playlist, 1, rng.randrange(4, 30) * PPQ * 4))

    def playlist_move() -> None:
        playlist = studio.playlist
        rows = project.playlist_items()
        if not len(rows):
            return
        row = rows[rng.randrange(len(rows))]
        middle = int(row["start"]) + int(row["length"]) // 2
        playlist.set_tool("draw")
        ui_input.drag(
            playlist.canvas,
            point(playlist, int(row["track"]), middle),
            point(playlist, int(row["track"]), middle + rng.choice((-1, 1)) * PPQ * 4),
        )

    def mixer_insert() -> None:
        studio.mixer.add_insert()

    def mixer_slot() -> None:
        master = int(project.mixer_info()["master"])
        session.build_and_execute("AddSlot", master, rng.choice(("EQ", "Delay", "Reverb")))

    def knob_turn() -> None:
        channels = project.channels_info()
        channel = int(channels[rng.randrange(len(channels))]["id"])
        studio.instrument.show_channel(channel)
        editors = studio.instrument.findChildren(GenericEditor)
        if not editors or not editors[0].controls:
            return
        editor = editors[0]
        name = rng.choice(sorted(editor.controls))
        descriptor = editor.descriptors[name]
        low, high = float(descriptor["min"]), float(descriptor["max"])
        value = low + (high - low) * rng.random()
        if str(descriptor["scale"]) == "stepped" or str(descriptor["unit"]) in ("count", "bool"):
            value = float(round(value))
        session.seal()  # a press on the knob
        editor.target.turn(name, value)

    def automate() -> None:
        channel = int(project.channels_info()[0]["id"])
        studio.instrument.show_channel(channel)
        editor = studio.instrument.findChildren(GenericEditor)[0]
        name = rng.choice(sorted(editor.controls))
        menu = editor.context_menu(name, editor.descriptors[name])
        next(a for a in menu.actions() if a.text() == "Create automation").trigger()

    def select_and_delete() -> None:
        rows = project.playlist_items()
        if len(rows) > 1:
            item = int(rows[rng.randrange(len(rows))]["item"])
            studio.selection.select(Kind.CLIPS, [item], Context())
            studio.registry.trigger("edit.delete")

    def duplicate_channel() -> None:
        channels = project.channels_info()
        studio.selection.select(Kind.CHANNELS, [int(channels[-1]["id"])], Context())
        studio.registry.trigger("edit.duplicate")

    def channel_volume() -> None:
        rows = studio.channel_rack.rows
        session.seal()
        studio.channel_rack._field_changed(rows[-1].channel, "volume", rng.random())

    return [
        rack_add_channel,
        rack_step,
        rack_step,
        playlist_place,
        playlist_move,
        mixer_insert,
        mixer_slot,
        knob_turn,
        knob_turn,
        automate,
        select_and_delete,
        duplicate_channel,
        channel_volume,
    ]


def test_undo_to_empty_from_every_panel(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    rng = random.Random(6)
    ready(studio)
    session, project = studio.session, studio.project
    start = project.dumps()
    ops = operations(studio, rng)
    taken: dict[str, int] = {}
    for _ in range(STEPS):
        op = rng.choice(ops)
        op()
        taken[op.__name__] = taken.get(op.__name__, 0) + 1
    finish = project.dumps()
    assert finish != start
    assert len(taken) == len({op.__name__ for op in ops}), f"not every operation ran: {taken}"
    depth = project.undo_depth()
    assert depth > STEPS // 2
    # Undo to empty through the Edit menu's action, as a user would.
    while studio.registry.trigger("edit.undo"):
        pass
    assert project.undo_depth() == 0
    assert project.dumps() == start
    # Every panel re-read the empty project: nothing left over from the walk.
    assert len(studio.channel_rack.rows) == 1
    assert len(studio.mixer.strips) == 1
    assert len(studio.project.playlist_items()) == 1
    # And forward again to exactly where the walk ended.
    while session.redo():
        pass
    assert project.dumps() == finish
    assert isinstance(studio.findChildren(QWidget), list)
    assert np.isfinite(project.content_length())
