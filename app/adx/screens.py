"""Which monitor the main window opens on, for runs that set ``ADX_SCREEN``.

Tests that open a real window (the scenarios, check_launcher, bench_ui, the GPU perf
drivers) set ``ADX_SCREEN=secondary`` so the app stays off the monitor the user is
working on. :func:`keep_windows_on_screen` extends that to every window the app shows
later: plugin editors and dialogs (menus, tooltips and floating docks follow on their
own). Values: ``secondary`` (the first screen that is not the primary; the primary when
there is only one), or a screen index as Qt lists them. Unset, the window opens wherever
Qt and the saved layout put it.

``ADX_BEHIND=1`` (also set by the test driver) sends those windows to the bottom of the
stack and hands the foreground back to whatever the user had, so a test run neither
covers their windows nor takes their typing. Qt is then told the window is active
(QApplication.setActiveWindow), which is all synthesized input needs: keys reach the
focus widget and window shortcuts match. Frame-rate benches leave it off: Windows may
throttle presenting a window that is fully covered.
"""

from __future__ import annotations

import os
import sys

from PySide6.QtCore import QEvent, QObject, Qt, QTimer
from PySide6.QtGui import QGuiApplication, QScreen
from PySide6.QtWidgets import QApplication, QWidget

__all__ = ["behind_requested", "keep_windows_on_screen", "place_on_screen", "requested_screen"]


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


def behind_requested() -> bool:
    """Whether ``ADX_BEHIND`` asks for windows to open behind the others."""
    return os.environ.get("ADX_BEHIND", "") not in ("", "0")


def _foreground() -> int:
    if sys.platform != "win32":
        return 0
    import ctypes

    return int(ctypes.windll.user32.GetForegroundWindow() or 0)


def _send_back(window: QWidget, previous: int) -> None:
    """Bottom of the stack, the user's foreground restored, and active to Qt alone."""
    if sys.platform == "win32" and window.isVisible():
        import ctypes

        user32 = ctypes.windll.user32
        user32.SetWindowPos.argtypes = [ctypes.c_void_p] * 2 + [ctypes.c_int] * 4 + [ctypes.c_uint]
        handle = int(window.winId())
        # HWND_BOTTOM = 1; SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE = 0x13.
        user32.SetWindowPos(handle, 1, 0, 0, 0, 0, 0x13)
        if previous and previous != handle and _foreground() == handle:
            user32.SetForegroundWindow(previous)
            # Windows posts the deactivation that follows; activate to Qt after it.
            QTimer.singleShot(100, lambda: _activate(window))
            return
    _activate(window)


def _activate(window: QWidget) -> None:
    if window.isVisible():
        QApplication.setActiveWindow(window)


def _reactivate() -> None:
    if QApplication.activeWindow() is not None:
        return
    top = QApplication.activeModalWidget()
    if top is None:
        shown = [w for w in QApplication.topLevelWidgets() if w.isVisible() and w.isWindow()]
        top = next((w for w in shown if w.windowType() == Qt.WindowType.Window), None)
    if top is not None:
        QApplication.setActiveWindow(top)


class _Keeper(QObject):
    """Moves each top-level window onto the requested screen as it is shown, and with
    ``ADX_BEHIND`` keeps it from taking focus or coming to the front."""

    def __init__(self, screen: QScreen, behind: bool, parent: QObject) -> None:
        super().__init__(parent)
        self.screen = screen
        self.behind = behind
        # What the user had in front before adX started: given back after each show.
        self.previous = _foreground()

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802 - Qt's name
        if (
            event.type() == QEvent.Type.Show
            and isinstance(watched, QWidget)
            and watched.windowType() in (Qt.WindowType.Window, Qt.WindowType.Dialog)
        ):
            if watched.screen() is not self.screen:
                place_on_screen(watched)
            if self.behind:
                # The Show event comes before the native show, so the attribute still
                # applies to this one; lowering waits until the window exists.
                watched.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
                if self.previous and _foreground() == self.previous:
                    # The user's window is in front: activate to Qt now, as Windows
                    # would have on show, so the next key (a dialog's Ctrl+A) is the
                    # dialog's and not a main-window shortcut.
                    _activate(watched)
                QTimer.singleShot(0, lambda w=watched: _send_back(w, self.previous))
        elif (
            self.behind
            and event.type() == QEvent.Type.Hide
            and isinstance(watched, QWidget)
            and watched.windowType() in (Qt.WindowType.Window, Qt.WindowType.Dialog)
        ):
            # Windows would reactivate the window underneath (a dialog's parent); the
            # app is not in front, so nothing does. Do it for Qt.
            QTimer.singleShot(0, _reactivate)
        return False


def keep_windows_on_screen(app: QApplication) -> None:
    """With ``ADX_SCREEN`` set, keep every window ``app`` shows on that screen."""
    screen = requested_screen()
    if screen is not None and app.findChild(_Keeper) is None:
        app.installEventFilter(_Keeper(screen, behind_requested(), app))
