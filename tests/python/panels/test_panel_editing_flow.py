"""The editing flow after the second user session (phase_6.md §11): Stop stops in place
and Return to start is its own button; the playhead is dragged on the ruler and snaps
to the grid; a paste lands at the playhead (not on top of what was copied) and the
pasted notes come selected; Ctrl+Shift+Z redoes, and undo and redo are buttons; a
rubber band shows its outline."""

from __future__ import annotations

import time
from collections.abc import Callable

import numpy as np
from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
from PySide6.QtGui import QKeySequence, QMouseEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from adx.engine_bridge import PPQ
from adx.mainwindow import MainWindow
from adx.panels.piano_roll.tools import CTRL
from adx.selection import Context, Kind
from adx.widgets.ruler import Ruler

#: The roll's grid box index of 1/4 (a beat).
QUARTER = 2


def settle(predicate: Callable[[], bool], seconds: float = 3.0) -> bool:
    """Pump events until ``predicate`` holds (the transport answers asynchronously)."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        QApplication.processEvents()
        if predicate():
            return True
        time.sleep(0.01)
    return predicate()


def roll_ready(studio: MainWindow) -> None:
    roll = studio.piano_roll
    roll.select_clip("P1", "Lead")
    roll.on_resize(1200.0, 600.0)
    roll.fit_pattern()


def draw(studio: MainWindow, tick: int, pitch: int) -> None:
    roll = studio.piano_roll
    roll.set_tool("draw")
    x, y = roll.view.x_of(tick) + 1, roll.view.y_of(pitch + 0.5)
    roll.input.pressed(x, y, 1, 0)
    roll.input.released(x, y, 1, 0)


def drag_on(ruler: Ruler, x0: float, x1: float) -> None:
    QTest.mousePress(
        ruler, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(int(x0), 20)
    )
    at = QPointF(x1, 20)
    move = QMouseEvent(
        QEvent.Type.MouseMove,
        at,
        ruler.mapToGlobal(at),
        Qt.MouseButton.NoButton,
        Qt.MouseButton.LeftButton,
        Qt.KeyboardModifier.NoModifier,
    )
    QApplication.sendEvent(ruler, move)
    QTest.mouseRelease(
        ruler, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(int(x1), 20)
    )


def test_stop_stays_put_and_start_rewinds(studio: MainWindow) -> None:
    engine = studio.engine
    engine.seek(8 * PPQ)
    assert settle(lambda: engine.position_ticks() == 8 * PPQ)
    studio.toggle_play()
    assert settle(lambda: engine.position_ticks() > 8 * PPQ)
    assert studio.registry.trigger("transport.stop")
    assert settle(lambda: engine.state() == "stopped")
    stopped_at = engine.position_ticks()
    assert stopped_at > 8 * PPQ
    time.sleep(0.1)
    assert engine.position_ticks() == stopped_at  # stopped in place, not rewound
    assert studio.registry.trigger("transport.rewind")
    assert settle(lambda: engine.position_ticks() == 0)
    assert studio.keymap.shortcut("transport.rewind") == "Home"
    assert studio.transport.rewind_action in studio.transport.actions()


def test_the_playhead_drags_and_snaps_to_the_grid(studio: MainWindow) -> None:
    roll_ready(studio)
    roll = studio.piano_roll
    roll.grid_box.setCurrentIndex(QUARTER)
    assert roll.snap_ticks() == PPQ
    ruler = roll.ruler
    ruler.resize(1000, ruler.height())
    ruler.set_view(0.0, 0.1)
    x = 1.4 * PPQ * 0.1
    QTest.mouseClick(
        ruler, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(int(x), 20)
    )
    assert roll.cursor_tick == PPQ
    QTest.mouseClick(
        ruler, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.AltModifier, QPoint(int(x), 20)
    )
    assert abs(roll.cursor_tick - 1.4 * PPQ) < 20  # Alt: no snap
    drag_on(ruler, x, 2.9 * PPQ * 0.1)
    assert roll.cursor_tick == 3 * PPQ
    # The pattern is placed at the start, so the transport went there too.
    assert settle(lambda: studio.engine.position_ticks() == 3 * PPQ)
    # The playlist's ruler snaps to the playlist's snap (a beat by default).
    studio.playlist.ruler.set_view(0.0, 0.1)
    assert studio.playlist.ruler.seek_tick(2.3 * PPQ * 0.1) == 2 * PPQ


def test_notes_paste_at_the_cursor_and_come_selected(studio: MainWindow) -> None:
    roll_ready(studio)
    roll = studio.piano_roll
    roll.grid_box.setCurrentIndex(QUARTER)
    draw(studio, 0, 60)
    draw(studio, PPQ, 62)
    roll.on_key(int(Qt.Key.Key_A.value), CTRL)
    assert studio.registry.trigger("edit.copy")
    roll.set_cursor(4 * PPQ + 30)  # off the grid: the paste snaps to it
    assert studio.registry.trigger("edit.paste")
    notes = studio.project.notes("P1", "Lead")
    assert sorted(zip(notes["start"].tolist(), notes["pitch"].tolist(), strict=True)) == [
        (0, 60),
        (PPQ, 62),
        (4 * PPQ, 60),
        (5 * PPQ, 62),
    ]
    assert roll.model is not None
    pasted = notes[notes["start"] >= 4 * PPQ]["id"]
    assert sorted(roll.model.selection.tolist()) == sorted(pasted.tolist())
    assert studio.selection.kind is Kind.NOTES
    # One undo step takes the paste away.
    studio.undo()
    assert len(studio.project.notes("P1", "Lead")) == 2


def test_clips_paste_at_the_playhead(studio: MainWindow) -> None:
    project = studio.project
    assert project is not None
    first = int(project.playlist_items()["item"][0])
    studio.selection.select(Kind.CLIPS, [first], Context())
    assert studio.registry.trigger("edit.copy")
    studio.engine.seek(8 * PPQ + 30)
    assert settle(lambda: studio.engine.position_ticks() == 8 * PPQ + 30)
    assert studio.registry.trigger("edit.paste")
    starts = sorted(int(s) for s in project.playlist_items()["start"])
    assert starts == [0, 8 * PPQ]


def test_the_latest_copy_wins(studio: MainWindow) -> None:
    """Notes copied, then a clip: Ctrl+V pastes the clip, though the notes' text is
    still on the system clipboard."""
    roll_ready(studio)
    draw(studio, 0, 60)
    studio.piano_roll.on_key(int(Qt.Key.Key_A.value), CTRL)
    assert studio.registry.trigger("edit.copy")
    project = studio.project
    assert project is not None
    first = int(project.playlist_items()["item"][0])
    studio.selection.select(Kind.CLIPS, [first], Context())
    assert studio.registry.trigger("edit.copy")
    assert not studio.ops.holds_notes()
    studio.engine.seek(16 * PPQ)
    assert settle(lambda: studio.engine.position_ticks() == 16 * PPQ)
    assert studio.registry.trigger("edit.paste")
    assert len(project.playlist_items()) == 2
    assert len(project.notes("P1", "Lead")) == 1


def test_redo_is_ctrl_shift_z_and_both_are_buttons(studio: MainWindow) -> None:
    redo = studio.registry.action("edit.redo")
    keys = [key.toString() for key in redo.shortcuts()]
    assert keys == ["Ctrl+Shift+Z", "Ctrl+Y"]
    assert studio.registry.action("edit.undo").shortcut() == QKeySequence("Ctrl+Z")
    bar = studio.transport.actions()
    assert studio.registry.action("edit.undo") in bar
    assert redo in bar
    # The buttons are the menu's actions: a change, then undo, then redo.
    studio.channel_rack.add_channel()
    channels = len(studio.project.channels())
    studio.registry.action("edit.undo").trigger()
    assert len(studio.project.channels()) == channels - 1
    redo.trigger()
    assert len(studio.project.channels()) == channels


def test_a_rubber_band_has_an_outline(studio: MainWindow) -> None:
    roll_ready(studio)
    roll = studio.piano_roll
    roll.set_tool("select")
    roll.input.pressed(100.0, 100.0, 1, 0)
    roll.input.moved(300.0, 250.0, 1, 0)
    band = roll.root.property("band")
    band = band.toVariant() if hasattr(band, "toVariant") else band
    assert np.allclose(list(band), [100.0, 100.0, 300.0, 250.0])
    roll.input.released(300.0, 250.0, 1, 0)
    band = roll.root.property("band")
    band = band.toVariant() if hasattr(band, "toVariant") else band
    assert len(band) == 0
    # The playlist's band too, and its toolbar shows the tool a key chose.
    playlist = studio.playlist
    playlist.set_tool("select")
    assert playlist._tool_actions["select"].isChecked()
    playlist.canvas.set_selection_rect((10.0, 10.0, 50.0, 40.0))
    band = playlist.canvas.root.property("band")
    band = band.toVariant() if hasattr(band, "toVariant") else band
    assert len(band) == 4
