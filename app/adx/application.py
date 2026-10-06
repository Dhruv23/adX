"""The application: QApplication, the engine's lifetime, global error handling.

The engine opens the default audio device. Where there is none (a CI runner, a machine
with its device taken), it falls back to the null backend - a real audio thread on a
timer - and says so in the status bar, so the application still plays, meters and
moves its playhead.
"""

from __future__ import annotations

import sys
import traceback
from types import TracebackType

from PySide6.QtCore import QSettings
from PySide6.QtWidgets import QApplication, QMessageBox

from adx import engine_bridge
from adx.engine_bridge import Engine, QuickPluginError
from adx.mainwindow import MainWindow


def start_engine(null_audio: bool) -> tuple[Engine, str]:
    """An engine with its stream started, and a note on which backend it uses."""
    if not null_audio:
        engine = Engine(null_backend=False)
        try:
            engine.start()
            return engine, "audio device open"
        except RuntimeError as error:
            fallback = f"no audio device ({error}); rendering on a timer instead"
    else:
        fallback = "null audio backend (--null-audio)"
    engine = Engine(null_backend=True)
    engine.start()
    return engine, fallback


def _install_excepthook() -> None:
    """Report an uncaught exception instead of dying with it.

    A Qt slot that raises would otherwise print and carry on, or abort - neither is
    something a user can act on. The traceback still goes to stderr.
    """
    shown = [False]

    def hook(kind: type[BaseException], value: BaseException, trace: TracebackType | None) -> None:
        traceback.print_exception(kind, value, trace)
        engine_bridge.errors.report("error", value)
        if not shown[0] and QApplication.instance() is not None:
            shown[0] = True
            QMessageBox.critical(None, "adX", f"{kind.__name__}: {value}\n\nDetails are on stderr.")
            shown[0] = False

    sys.excepthook = hook


def create(
    null_audio: bool = False, settings: QSettings | None = None
) -> tuple[QApplication, MainWindow]:
    """The application and its window, not yet shown. Tests drive this directly."""
    existing = QApplication.instance()
    app = existing if isinstance(existing, QApplication) else QApplication(sys.argv[:1])
    app.setApplicationName("adX")
    app.setOrganizationName("adX")
    engine_bridge.load_quick_plugin()
    engine, note = start_engine(null_audio)
    window = MainWindow(engine, settings)
    window.statusBar().showMessage(note)
    return app, window


def run(project: str | None = None, null_audio: bool = False) -> int:
    """Open the application; returns its exit status."""
    _install_excepthook()
    try:
        app, window = create(null_audio)
    except QuickPluginError as error:
        if QApplication.instance() is None:
            QApplication(sys.argv[:1])
        QMessageBox.critical(None, "adX", str(error))
        return 1
    if project:
        window.open_project(project)
    else:
        window.new_project()
    window.show()
    status = int(app.exec())
    window.engine.stop()
    _teardown(window)
    return status


def _teardown(window: MainWindow) -> None:
    """Destroy the window while QApplication still exists, in a defined order, rather
    than leaving it (its signal connections make reference cycles) to the garbage
    collector at interpreter exit."""
    import gc

    import shiboken6

    if shiboken6.isValid(window):
        shiboken6.delete(window)
    gc.collect()
