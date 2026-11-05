"""The application: QApplication, the engine's lifetime, global error handling.

The engine opens the default audio device. Where there is none (a CI runner, a machine
with its device taken), it falls back to the null backend - a real audio thread on a
timer - and says so in the status bar, so the application still plays, meters and
moves its playhead.
"""

from __future__ import annotations

import json
import os
import sys
import traceback
from types import TracebackType

from PySide6.QtCore import QSettings, QTimer
from PySide6.QtWidgets import QApplication, QMessageBox

from adx import engine_bridge, frame_clock
from adx.control import server as control_server
from adx.engine_bridge import Engine, QuickPluginError
from adx.mainwindow import MainWindow
from adx.preferences import Preferences
from adx.screens import keep_windows_on_screen, place_on_screen


def start_engine(
    null_audio: bool,
    input_channels: int = 0,
    sample_rate: int = 48000,
    block_frames: int = 256,
    exclusive: bool = False,
) -> tuple[Engine, str]:
    """An engine with its stream started, and a note on which backend it uses. With
    ``input_channels``, the default input is opened too, for recording; a device that
    will not open with an input is opened without one. ``exclusive`` tries WASAPI
    exclusive mode first (output only, so no recording input) and falls back to shared
    mode when the device refuses it."""
    if not null_audio:
        error: RuntimeError | None = None
        if exclusive:
            engine = Engine(sample_rate=sample_rate, block_frames=block_frames, exclusive=True)
            try:
                engine.start()
            except RuntimeError as failed:
                error = failed
            else:
                stream = engine.stream
                note = (
                    f"audio device open, exclusive mode ({stream['block_frames']} frames, "
                    f"{stream['output_latency_ms']:.1f} ms)"
                )
                return engine, note + ("; no recording input" if input_channels else "")
        shared_note = f"; exclusive mode refused ({error})" if exclusive else ""
        for inputs in dict.fromkeys((input_channels, 0)):
            engine = Engine(
                null_backend=False,
                sample_rate=sample_rate,
                block_frames=block_frames,
                input_channels=inputs,
            )
            try:
                engine.start()
            except RuntimeError as failed:
                error = failed
                continue
            if inputs == input_channels:
                return engine, "audio device open" + shared_note
            return engine, f"audio device open; no recording input ({error})" + shared_note
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
        frame_clock.errors.report("error", value)
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
    keep_windows_on_screen(app)
    engine_bridge.load_quick_plugin()
    if settings is None:
        # A driven run (phase_10.5.md) keeps its layout and recent files out of the user's.
        folder = os.environ.get("ADX_SETTINGS_DIR")
        settings = (
            QSettings(os.path.join(folder, "settings.ini"), QSettings.Format.IniFormat)
            if folder
            else QSettings("adX", "adX")
        )
    prefs = Preferences.load(settings)
    engine, note = start_engine(
        null_audio,
        2 if prefs.record_input else 0,
        sample_rate=prefs.sample_rate,
        block_frames=prefs.buffer_frames,
        exclusive=prefs.exclusive_audio,
    )
    window = MainWindow(engine, settings)
    window.statusBar().showMessage(note)
    return app, window


def run(
    project: str | None = None,
    null_audio: bool = False,
    control: bool = False,
    control_file: str | None = None,
) -> int:
    """Open the application; returns its exit status. ``control`` (or ``ADX_CONTROL``)
    opens the agent's control channel (phase_10.5.md 4.2); it is off otherwise."""
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
    place_on_screen(window)
    window.show()
    servers: list[control_server.ControlServer] = []
    if control or control_server.requested():
        # From the event loop, after the window's first layout pass (showEvent queues
        # it): a driver connecting earlier read a half-laid-out tree under load.
        def start_control() -> None:
            servers.append(control_server.ControlServer(window, control_file))
            window.control_server = servers[0]

        QTimer.singleShot(0, start_control)
    smoke = os.environ.get("ADX_SMOKE")
    if not smoke and not os.environ.get("ADX_NO_QUICKSTART"):
        window.maybe_show_quickstart()
    if smoke:
        _smoke_after_start(app, window, smoke)
    status = int(app.exec())
    for server in servers:
        server.close()
    window.engine.stop()
    _teardown(window)
    return status


def _smoke_after_start(app: QApplication, window: MainWindow, report: str) -> None:
    """The release check (tools/build_exe.py): once the window has run for a moment,
    write what came up to ``report`` and quit with status 0. A windowed executable has
    no console, so a file is how it says that the whole application started."""

    def done() -> None:
        facts = {
            "version": engine_bridge.engine_version(),
            "panels": sorted(p.objectName() for p in window.panels),
            "visible": window.isVisible(),
            "channels": len(window.session.require().channels_info()),
            "status": window.statusBar().currentMessage(),
            "pid": os.getpid(),
            "control": getattr(window, "control_server", None) is not None,
        }
        with open(report, "w", encoding="utf-8") as out:
            json.dump(facts, out)
        window.saved_revision = window.session.require().revision()
        app.quit()

    QTimer.singleShot(1500, done)


def _teardown(window: MainWindow) -> None:
    """Destroy the window while QApplication still exists, in a defined order, rather
    than leaving it (its signal connections make reference cycles) to the garbage
    collector at interpreter exit."""
    import gc

    import shiboken6

    if shiboken6.isValid(window):
        shiboken6.delete(window)
    gc.collect()
