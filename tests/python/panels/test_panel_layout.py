"""Layout persists and resets (phase_6.md 6), and the theme reaches every panel."""

from __future__ import annotations

import pathlib

from PySide6.QtCore import QSettings, Qt
from PySide6.QtWidgets import QApplication

from adx.application import create
from adx.mainwindow import MainWindow
from conftest import dispose


def test_layout_persists_and_resets(studio: MainWindow, tmp_path: pathlib.Path) -> None:
    studio.show()
    QApplication.processEvents()
    # Rearrange: the mixer floats, the history panel closes, the browser moves right.
    studio.mixer.setFloating(True)
    studio.history.hide()
    studio.addDockWidget(Qt.DockWidgetArea.RightDockWidgetArea, studio.browser)
    QApplication.processEvents()
    studio.save_layout()
    studio.settings.sync()

    # A second window over the same settings comes back the same way.
    settings = QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)
    _app, again = create(null_audio=True, settings=settings)
    try:
        again.clock.stop()
        again.show()
        QApplication.processEvents()
        assert again.mixer.isFloating()
        assert not again.history.isVisible()
        assert again.dockWidgetArea(again.browser) == Qt.DockWidgetArea.RightDockWidgetArea
        # Reset: every panel docked and visible, in its default area.
        again.reset_layout()
        QApplication.processEvents()
        for panel in again.panels:
            assert not panel.isFloating(), panel.objectName()
            assert again.dockWidgetArea(panel) == panel.default_area, panel.objectName()
        assert again.history.isVisibleTo(again) or again.tabifiedDockWidgets(again.history)
    finally:
        again.clock.stop()
        again.engine.stop()
        again.close()
        dispose(again)


def test_theme_switch_reaches_every_panel(studio: MainWindow) -> None:
    for name in ("light", "dark"):
        studio.set_theme_name(name)
        assert studio.theme.name == name
    assert QApplication.instance().styleSheet()


def test_the_window_does_not_start_in_a_text_field(studio: MainWindow) -> None:
    """A focused text field's caret repaints every 500 ms, and in this window each
    repaint costs a vsync: the GPU measurement dropped a frame per blink until the
    tempo box stopped taking focus at startup (phase_6.md §10)."""
    from PySide6.QtWidgets import QAbstractSpinBox, QLineEdit

    studio.show()
    for _ in range(5):
        QApplication.processEvents()
    focused = QApplication.focusWidget()
    assert not isinstance(focused, QLineEdit | QAbstractSpinBox), focused
    tempo = studio.transport.tempo
    assert tempo.focusPolicy() == Qt.FocusPolicy.ClickFocus
