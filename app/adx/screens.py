"""Which monitor the main window opens on, for runs that set ``ADX_SCREEN``.

Tests that open a real window (the scenarios, check_launcher, bench_ui, the GPU perf
drivers) set ``ADX_SCREEN=secondary`` so the app stays off the monitor the user is
working on. :func:`keep_windows_on_screen` extends that to every window the app shows
later: plugin editors and dialogs (menus, tooltips and floating docks follow on their
own). Values: ``secondary`` (the first screen that is not the primary; the primary when
there is only one), or a screen index as Qt lists them. Unset, the window opens wherever
Qt and the saved layout put it.
"""

from __future__ import annotations

import os

from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtGui import QGuiApplication, QScreen
from PySide6.QtWidgets import QApplication, QWidget

__all__ = ["keep_windows_on_screen", "place_on_screen", "requested_screen"]


def requested_screen() -> QScreen | None:
    """The screen ``ADX_SCREEN`` names, or None when it is unset."""
    wanted = os.environ.get("ADX_SCREEN", "").strip().lower()
    if not wanted:
        return None
    screens = QGuiApplication.screens()
    primary = QGuiApplication.primaryScreen()
    if wanted == "secondary":
        others = [s for s in screens if s is not primary]
        return others[0] if others else primary
    try:
        return screens[int(wanted)]
    except (ValueError, IndexError):
        return primary


def place_on_screen(window: QWidget) -> None:
    """Move ``window`` onto the requested screen, sized to fit it. Call before show()."""
    screen = requested_screen()
    if screen is None:
        return
    area = screen.availableGeometry()
    width = min(window.width(), area.width())
    height = min(window.height(), area.height())
    window.resize(width, height)
    window.move(area.x() + (area.width() - width) // 2, area.y() + (area.height() - height) // 2)
    handle = window.windowHandle()
    if handle is not None:
        handle.setScreen(screen)


class _Keeper(QObject):
    """Moves each top-level window onto the requested screen as it is first shown."""

    def __init__(self, screen: QScreen, parent: QObject) -> None:
        super().__init__(parent)
        self.screen = screen

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802 - Qt's name
        if (
            event.type() == QEvent.Type.Show
            and isinstance(watched, QWidget)
            and watched.windowType() in (Qt.WindowType.Window, Qt.WindowType.Dialog)
            and watched.screen() is not self.screen
        ):
            place_on_screen(watched)
        return False


def keep_windows_on_screen(app: QApplication) -> None:
    """With ``ADX_SCREEN`` set, keep every window ``app`` shows on that screen."""
    screen = requested_screen()
    if screen is not None and app.findChild(_Keeper) is None:
        app.installEventFilter(_Keeper(screen, app))
