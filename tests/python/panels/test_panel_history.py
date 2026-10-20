"""The history panel (phase_6.md 4.8, 5: ``history_jump``)."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtTest import QTest

from adx.mainwindow import MainWindow
from adx.panels.history.panel import HistoryPanel
from ui_input import user_press


def click_row(history: HistoryPanel, row: int) -> None:
    """A real mouse click on one row of the list."""
    widget = history.list
    widget.show()
    rect = widget.visualItemRect(widget.item(row))
    QTest.mouseClick(widget.viewport(), Qt.MouseButton.LeftButton, pos=rect.center())


def test_history_jump(studio: MainWindow) -> None:
    session = studio.session
    for name in ("A", "B", "C", "D"):
        session.build_and_execute("AddChannelNew", name, "additive", 0, "")
    history = studio.history
    history.resize(240, 400)
    assert studio.project.undo_depth() == 4
    names = lambda: [str(c["name"]) for c in studio.project.channels_info()]  # noqa: E731
    # Jump back to after the second step: exactly two done.
    click_row(history, 2)
    assert studio.project.undo_depth() == 2
    assert names() == ["Lead", "A", "B"]
    # Forward again, to the end.
    click_row(history, 4)
    assert names() == ["Lead", "A", "B", "C", "D"]
    # To the very start.
    click_row(history, 0)
    assert names() == ["Lead"]
    done, undone = studio.project.history_info()
    assert done == []
    assert len(undone) == 4


def test_history_coalesces_a_drag(studio: MainWindow) -> None:
    # A knob drag is a press, then many set_param calls: one history row.
    user_press(studio)
    for value in (0.1, 0.2, 0.3, 0.4, 0.5):
        studio.session.set_param("channel.Lead.volume", value)
    done, _undone = studio.project.history_info()
    assert len(done) == 1
    assert abs(studio.project.param_value("channel.Lead.volume") - 0.5) < 1e-6
    studio.session.undo()
    assert abs(studio.project.param_value("channel.Lead.volume") - 1.0) < 1e-6
