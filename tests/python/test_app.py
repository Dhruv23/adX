"""The shell end to end (phase_5.md 6): it launches, docks its panels, opens
suffocation.adx, plays it, and the playhead moves. Offscreen, on the null backend."""

from __future__ import annotations

import pathlib
import time

import pytest
from PySide6.QtCore import QSettings
from PySide6.QtWidgets import QApplication

from adx.application import create
from adx.theme.tokens import THEMES
from conftest import dispose


@pytest.fixture
def window(quick: None, tmp_path: pathlib.Path) -> object:
    settings = QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)
    _app, main = create(null_audio=True, settings=settings)
    yield main
    main.clock.stop()
    main.engine.stop()
    main.saved_revision = main.project.revision() if main.project else 0
    main.close()
    dispose(main)


def _pump(seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        QApplication.processEvents()
        time.sleep(0.005)


def test_opens_plays_and_moves_the_playhead(window: object, repo_root: pathlib.Path) -> None:
    from adx.mainwindow import MainWindow

    assert isinstance(window, MainWindow)
    window.show()
    assert window.open_project(str(repo_root / "docs/examples/suffocation.adx"))
    roll = window.piano_roll
    assert roll.model is not None
    assert len(roll.model.notes()) > 0
    assert {p.objectName() for p in window.panels} == {
        "browser",
        "piano_roll",
        "waveform",
        "meters",
    }

    window.toggle_play()
    _pump(0.6)
    assert window.clock.rolling
    first = window.clock.position_ticks
    _pump(0.4)
    assert window.clock.position_ticks > first > 0
    assert window.transport.position.text().strip() != ""
    assert window.clock.strips > 0  # meters are being read
    playhead = float(roll.item.property("playheadBeats"))
    assert playhead >= 0.0 or not len(roll._placements)
    window.stop()
    _pump(0.1)
    assert window.clock.position_ticks == 0 or not window.clock.rolling


def test_edit_undo_save_and_theme(window: object, tmp_path: pathlib.Path) -> None:
    from adx.mainwindow import MainWindow
    from adx.panels.piano_roll.tools import DrawTool, Pointer

    assert isinstance(window, MainWindow)
    window.new_project()
    roll = window.piano_roll
    assert roll.model is not None
    before = window.project.dumps() if window.project else ""
    view = roll.view
    tool = DrawTool()
    point = Pointer(view.x_of(3840.0), view.y_of(60.5))
    tool.press(roll.model, point)
    tool.release(roll.model, point)
    assert len(roll.model.notes()) == 1
    assert window._dirty()
    window.undo()
    assert window.project is not None
    assert window.project.dumps() == before
    window.redo()
    target = tmp_path / "saved.adx"
    window.path = str(target)
    assert window.save()
    assert "NOTES Lead" in target.read_text(encoding="utf-8")
    for theme in THEMES.values():
        window.set_theme(theme)
    window.reset_layout()
    window.save_layout()
    assert window.settings.value("window/state") is not None
