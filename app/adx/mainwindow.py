"""The shell: dockable panels, menus, the transport, and layout persistence
(phase_5.md 4.10).

Widgets for the chrome and Qt Quick for the hot surfaces: ``QDockWidget`` gives real
docking, tabbed docks, floating panels and saved layouts for free, which Qt Quick has
no equivalent of. The layout persists to QSettings, and View > Reset layout restores
the default - a saved layout that cannot be reset is a bug report per week.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PySide6.QtCore import QByteArray, QSettings, Qt, QTimer
from PySide6.QtGui import QAction, QCloseEvent, QKeySequence, QShowEvent
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QHBoxLayout,
    QMainWindow,
    QMessageBox,
    QWidget,
)

from adx import __version__, engine_bridge
from adx.engine_bridge import Engine, FrameClock, Project
from adx.panels.base import DockPanel
from adx.panels.browser import BrowserPanel
from adx.panels.piano_roll.panel import PianoRollPanel
from adx.panels.waveform.widget import WaveformPanel
from adx.theme.tokens import THEMES, Theme, stylesheet
from adx.transport_bar import TransportBar
from adx.widgets.meter import LevelMeter

#: Bump when the dock layout changes shape, so an old saved layout is not restored
#: into panels it does not describe.
LAYOUT_VERSION = 1

NEW_PROJECT = """[PROJECT]
ADX_VERSION=2

[CHANNEL Lead]
OUTPUT=insert.1

[PATTERN P1]
LENGTH=4:0:0

[PLAYLIST]
TRACK 1
  PATTERN P1 0:0:0

[MIXER]
INSERT 1 name="Master"
"""


class MetersPanel(DockPanel):
    """One level meter per mixer strip, fed by the frame clock."""

    panel_id = "meters"
    title = "Meters"
    default_area = Qt.DockWidgetArea.BottomDockWidgetArea

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._body = QWidget()
        self._layout = QHBoxLayout(self._body)
        self._layout.setContentsMargins(6, 6, 6, 6)
        self._meters: list[LevelMeter] = []
        self.setWidget(self._body)

    def show_levels(self, levels: np.ndarray, strips: int) -> None:
        """Rows from Engine.frame(): insert id, peak L/R, RMS L/R, ..."""
        while len(self._meters) < strips:
            meter = LevelMeter()
            self._meters.append(meter)
            self._layout.addWidget(meter)
        for index, meter in enumerate(self._meters):
            if index < strips:
                row = levels[index]
                meter.setToolTip(f"insert {int(row[0])}")
                meter.set_levels(float(row[1]), float(row[2]), float(row[3]), float(row[4]))
            meter.setVisible(index < strips)

    def apply_theme(self, theme: Theme) -> None:
        """Recolour."""
        for meter in self._meters:
            meter.set_theme(theme)


class MainWindow(QMainWindow):
    """The application window."""

    def __init__(self, engine: Engine, settings: QSettings | None = None) -> None:
        super().__init__()
        self.engine = engine
        self.settings = settings or QSettings("adX", "adX")
        self.project: Project | None = None
        self.path: str | None = None
        self.saved_revision = 0
        self.theme: Theme = THEMES["dark"]
        self.setWindowTitle("adX")
        self.setDockNestingEnabled(True)
        self.transport = TransportBar(self)
        self.addToolBar(self.transport)
        self.piano_roll = PianoRollPanel(self)
        self.browser = BrowserPanel(str(Path.cwd()), self)
        self.waveform = WaveformPanel(self)
        self.meters = MetersPanel(self)
        self.panels: list[DockPanel] = [self.browser, self.piano_roll, self.waveform, self.meters]
        self.resize(1440, 900)
        self._place_panels()
        # Dock sizes only mean something once the window has its size; the first show
        # applies them, unless a saved layout was restored.
        self._needs_default_sizes = True
        self._build_menus()
        self._connect()
        self.clock = FrameClock(engine, parent=self)
        self.clock.frame.connect(self._on_frame)
        self.statusBar().showMessage("Ready")
        engine_bridge.errors.error.connect(
            lambda where, message: self.statusBar().showMessage(f"{where}: {message}")
        )
        self.restore_layout()
        self.clock.start()

    # --- construction ---------------------------------------------------------------

    def _action(
        self, menu_name: str, text: str, slot: object, shortcut: str | None = None
    ) -> QAction:
        menu = self._menus[menu_name]
        action = QAction(text, self)
        if shortcut:
            action.setShortcut(QKeySequence(shortcut))
            action.setShortcutContext(Qt.ShortcutContext.ApplicationShortcut)
        action.triggered.connect(slot)
        menu.addAction(action)
        return action

    def _build_menus(self) -> None:
        bar = self.menuBar()
        self._menus = {name: bar.addMenu(name) for name in ("&File", "&Edit", "&View", "&Help")}
        self._action("&File", "&New", self.new_project, "Ctrl+N")
        self._action("&File", "&Open…", self.open_dialog, "Ctrl+O")
        self._action("&File", "&Save", self.save, "Ctrl+S")
        self._action("&File", "Save &As…", self.save_as, "Ctrl+Shift+S")
        self._menus["&File"].addSeparator()
        self._action("&File", "&Quit", self.close, "Ctrl+Q")
        self.undo_action = self._action("&Edit", "&Undo", self.undo, "Ctrl+Z")
        self.redo_action = self._action("&Edit", "&Redo", self.redo, "Ctrl+Y")
        view = self._menus["&View"]
        for panel in self.panels:
            view.addAction(panel.toggleViewAction())
        view.addSeparator()
        self._action("&View", "&Reset layout", self.reset_layout)
        for name in THEMES:
            self._action(
                "&View", f"{name.title()} theme", lambda _c=False, n=name: self.set_theme(THEMES[n])
            )
        self._action("&Help", "&About adX", self._about)

    def _connect(self) -> None:
        self.transport.play_requested.connect(self.toggle_play)
        self.transport.stop_requested.connect(self.stop)
        self.transport.loop_toggled.connect(self._loop)
        self.transport.tempo_changed.connect(self._tempo)
        self.browser.project_activated.connect(self.open_project)
        self.browser.audio_activated.connect(self.waveform.waveform.open)
        self.piano_roll.seek = self.engine.seek

    # --- projects -------------------------------------------------------------------

    def _adopt(self, project: Project, path: str | None) -> None:
        self.project = project
        self.path = path
        self.saved_revision = project.revision()
        self.engine.set_project(project)
        self.piano_roll.set_project(project, on_edit=self.project_edited)
        self.transport.set_tempo(project.bpm_at(0))
        self.transport.meter = project.meter_at(0)
        if path:
            self.browser.set_root(str(Path(path).parent))
            self.settings.setValue("last_project", path)
        self._update_title()

    def new_project(self) -> None:
        """A fresh project: one channel, one empty pattern."""
        if not self._may_discard():
            return
        project, _diagnostics = Project.loads(NEW_PROJECT)
        self._adopt(project, None)

    def open_dialog(self) -> None:
        """Ask for a file, then open it."""
        start = str(self.settings.value("last_dir", str(Path.cwd())))
        path, _filter = QFileDialog.getOpenFileName(
            self, "Open project", start, "adX projects (*.adx)"
        )
        if path:
            self.settings.setValue("last_dir", str(Path(path).parent))
            self.open_project(path)

    def open_project(self, path: str) -> bool:
        """Open a .adx file; report errors; keep the current project if it fails."""
        if not self._may_discard():
            return False
        try:
            project, diagnostics = Project.load(path)
        except (OSError, RuntimeError, ValueError) as error:
            QMessageBox.warning(self, "adX", f"Cannot open {path}:\n{error}")
            return False
        errors = [d for d in diagnostics if str(d["severity"]) == "error"]
        if errors:
            first = errors[0]
            self.statusBar().showMessage(
                f"{Path(path).name}: {len(errors)} error(s); "
                f"first at line {first['line']}: {first['message']}"
            )
        self._adopt(project, path)
        return True

    def save(self) -> bool:
        """Save in place (asking for a name the first time)."""
        if self.project is None:
            return False
        if self.path is None:
            return self.save_as()
        self.project.save(self.path)
        self.saved_revision = self.project.revision()
        self._update_title()
        return True

    def save_as(self) -> bool:
        """Ask for a name, then save."""
        if self.project is None:
            return False
        path, _filter = QFileDialog.getSaveFileName(
            self, "Save project", self.path or "untitled.adx", "adX projects (*.adx)"
        )
        if not path:
            return False
        self.path = path
        return self.save()

    def _dirty(self) -> bool:
        return self.project is not None and self.project.revision() != self.saved_revision

    def _may_discard(self) -> bool:
        if not self._dirty():
            return True
        answer = QMessageBox.question(
            self,
            "adX",
            "Save changes to the current project?",
            QMessageBox.StandardButton.Save
            | QMessageBox.StandardButton.Discard
            | QMessageBox.StandardButton.Cancel,
        )
        if answer == QMessageBox.StandardButton.Save:
            return self.save()
        return answer == QMessageBox.StandardButton.Discard

    def _update_title(self) -> None:
        name = Path(self.path).name if self.path else "untitled"
        self.setWindowTitle(f"{name}{' •' if self._dirty() else ''} - adX {__version__}")

    def project_edited(self) -> None:
        """After every command: the audio thread catches up, incrementally."""
        if self.project is not None:
            self.engine.commit(self.project)
        self._update_title()

    def undo(self) -> None:
        """Undo one step, everywhere."""
        if self.project is not None and self.project.undo():
            self.project_edited()
            self.piano_roll.refresh()

    def redo(self) -> None:
        """Redo one step, everywhere."""
        if self.project is not None and self.project.redo():
            self.project_edited()
            self.piano_roll.refresh()

    # --- transport ------------------------------------------------------------------

    def toggle_play(self) -> None:
        """Play, or pause where it is."""
        if self.clock.rolling:
            self.engine.stop_playback()
        else:
            self.engine.play()

    def stop(self) -> None:
        """Stop and return to the start."""
        self.engine.stop_playback()
        self.engine.seek(0)

    def _loop(self, enabled: bool) -> None:
        if self.project is not None:
            length = int(self.project.info()["length_ticks"])
            self.engine.set_loop(0, max(length, engine_bridge.PPQ * 4), enabled)

    def _tempo(self, bpm: float) -> None:
        if self.project is None:
            return
        self.project.execute(engine_bridge.commands.set_tempo(bpm, 0))
        self.project_edited()
        self.piano_roll.refresh()

    def _on_frame(self, ticks: int, rolling: bool) -> None:
        """The 60 Hz frame. The clock made the one engine call; this only draws."""
        self.transport.show_position(ticks, self.clock.position_seconds)
        self.transport.set_playing(rolling)
        self.piano_roll.frame(ticks, rolling)
        self.meters.show_levels(self.clock.levels, self.clock.strips)

    # --- layout and theme -----------------------------------------------------------

    def restore_layout(self) -> None:
        """Restore the saved window, docks and panel state (tolerating none)."""
        geometry = self.settings.value("window/geometry")
        state = self.settings.value("window/state")
        if isinstance(geometry, QByteArray):
            self.restoreGeometry(geometry)
        if isinstance(state, QByteArray) and self.restoreState(state, LAYOUT_VERSION):
            self._needs_default_sizes = False
        for panel in self.panels:
            panel.restore_state(self.settings)
        self.set_theme(THEMES.get(str(self.settings.value("theme", "dark")), THEMES["dark"]))

    def save_layout(self) -> None:
        """Persist the window, docks and panel state."""
        self.settings.setValue("window/geometry", self.saveGeometry())
        self.settings.setValue("window/state", self.saveState(LAYOUT_VERSION))
        for panel in self.panels:
            panel.save_state(self.settings)
        self.settings.setValue("theme", self.theme.name)

    def _place_panels(self) -> None:
        """The default arrangement: the piano roll large, the browser beside it, the
        waveform and the meters along the bottom."""
        for panel in self.panels:
            panel.setFloating(False)
            self.addDockWidget(panel.default_area, panel)
            panel.show()
        self.splitDockWidget(self.waveform, self.meters, Qt.Orientation.Horizontal)

    def _size_panels(self) -> None:
        width, height = max(800, self.width()), max(500, self.height())
        self.resizeDocks(
            [self.browser, self.piano_roll], [240, width - 240], Qt.Orientation.Horizontal
        )
        self.resizeDocks(
            [self.piano_roll, self.waveform],
            [int(height * 0.66), int(height * 0.2)],
            Qt.Orientation.Vertical,
        )
        self.resizeDocks(
            [self.waveform, self.meters], [width - 420, 420], Qt.Orientation.Horizontal
        )

    def showEvent(self, event: QShowEvent) -> None:  # noqa: N802 - Qt's name
        super().showEvent(event)
        if self._needs_default_sizes:
            self._needs_default_sizes = False
            QTimer.singleShot(0, self._size_panels)

    def reset_layout(self) -> None:
        """Back to the default dock layout, every panel visible."""
        self._place_panels()
        self._size_panels()
        self.settings.remove("window/state")

    def set_theme(self, theme: Theme) -> None:
        """Switch theme: stylesheet, QML palette and role colours. No geometry rebuilds."""
        self.theme = theme
        app = QApplication.instance()
        if isinstance(app, QApplication):
            app.setStyleSheet(stylesheet(theme))
        self.piano_roll.apply_theme(theme)
        self.waveform.waveform.apply_theme(theme)
        self.meters.apply_theme(theme)

    def _about(self) -> None:
        QMessageBox.about(
            self,
            "About adX",
            f"adX {__version__}\n"
            f"engine {engine_bridge.engine_version()} ({engine_bridge.git_sha()})",
        )

    def closeEvent(self, event: QCloseEvent) -> None:  # noqa: N802 - Qt's name
        if not self._may_discard():
            event.ignore()
            return
        self.clock.stop()
        self.save_layout()
        event.accept()
