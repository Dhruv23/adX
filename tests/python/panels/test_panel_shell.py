"""The shell's usability fixes after the first user session (phase_6.md §11): every
panel resizes, the editors share the big middle area, F-keys and F11 move between and
maximise panels, the rack's Notes button opens the piano roll, nothing clears to white,
and the quick start is one key away."""

from __future__ import annotations

from PySide6.QtGui import QColor
from PySide6.QtWidgets import QApplication

from adx.mainwindow import MainWindow
from adx.panels.geometry_view import GeometryView

#: No panel may demand more than this: a panel's minimum pins the splitter beside it.
#: The instrument editor's 357 px minimum kept the piano roll from growing.
MAX_MIN_HEIGHT = 160
MAX_MIN_WIDTH = 240


def shown(studio: MainWindow) -> None:
    studio.resize(1600, 1000)
    studio.show()
    for _ in range(5):
        QApplication.processEvents()


def test_no_panel_pins_a_splitter(studio: MainWindow) -> None:
    shown(studio)
    for panel in studio.panels:
        hint = panel.minimumSizeHint()
        assert hint.height() <= MAX_MIN_HEIGHT, (panel.objectName(), hint.height())
        assert hint.width() <= MAX_MIN_WIDTH, (panel.objectName(), hint.width())


def test_the_editors_share_the_middle(studio: MainWindow) -> None:
    shown(studio)
    group = set(studio.tabifiedDockWidgets(studio.playlist))
    assert {studio.channel_rack, studio.piano_roll} <= group
    # The instrument column sits beside them, not under the piano roll.
    assert studio.instrument not in group
    assert studio.instrument.x() > studio.playlist.x() + studio.playlist.width() - 5


def test_f_keys_bring_panels_forward(studio: MainWindow) -> None:
    shown(studio)
    for action, panel in (
        ("view.piano_roll", studio.piano_roll),
        ("view.channel_rack", studio.channel_rack),
        ("view.playlist", studio.playlist),
    ):
        assert studio.registry.trigger(action)
        QApplication.processEvents()
        assert not panel.visibleRegion().isEmpty(), action
    keys = {a: studio.keymap.shortcut(a) for a in ("view.playlist", "view.piano_roll")}
    assert keys == {"view.playlist": "F5", "view.piano_roll": "F7"}


def test_f11_maximises_and_restores(studio: MainWindow) -> None:
    shown(studio)
    studio.show_panel(studio.piano_roll)
    QApplication.processEvents()
    width = studio.piano_roll.width()
    assert studio.registry.trigger("view.maximize")
    QApplication.processEvents()
    assert [p for p in studio.panels if p.isVisible()] == [studio.piano_roll]
    assert studio.piano_roll.width() > width
    assert studio.registry.trigger("view.maximize")
    QApplication.processEvents()
    assert all(p.isVisible() for p in (studio.mixer, studio.browser, studio.instrument))


def test_notes_button_opens_the_piano_roll(studio: MainWindow) -> None:
    shown(studio)
    rack = studio.channel_rack
    rack.select_pattern("P1")
    rack.rows[0].notes.click()
    QApplication.processEvents()
    assert studio.piano_roll.model is not None
    assert studio.piano_roll.model.channel == "Lead"
    assert not studio.piano_roll.visibleRegion().isEmpty()


def test_geometry_views_never_clear_to_white(studio: MainWindow) -> None:
    for view in studio.findChildren(GeometryView):
        assert view.quickWindow().color() == QColor(studio.theme.window), view.parent()


def test_quick_start_is_f1(studio: MainWindow) -> None:
    assert studio.keymap.shortcut("help.quickstart") == "F1"
    assert studio.registry.trigger("help.quickstart")
    assert studio._quickstart is not None
    assert studio._quickstart.isVisible()
    studio._quickstart.close()
