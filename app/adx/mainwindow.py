"""The shell: every panel, the transport, the menus, layout persistence (phase_5.md 4.10,
phase_6.md 2).

One :class:`~adx.session.Session` owns the open project and its engine; every panel edits
through it and refreshes when it says the project changed. One global
:class:`~adx.selection.Selection` and the edit operations over it (delete, copy, paste,
duplicate) serve every panel. Every menu item is an action from the registry
(``menus.py``), and the layout persists to QSettings - with View > Reset layout to undo
whatever a user did to it.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QByteArray, QEvent, QObject, QSettings, Qt, QTimer
from PySide6.QtGui import QCloseEvent, QKeyEvent, QShowEvent
from PySide6.QtWidgets import QApplication, QFileDialog, QMainWindow, QMessageBox

from adx import __version__, engine_bridge
from adx.actions import ActionRegistry
from adx.autosave import Autosave
from adx.edit_ops import EditOps
from adx.engine_bridge import PPQ, Engine, FrameClock, Project
from adx.menus import register_actions
from adx.panels.automation.panel import AutomationPanel
from adx.panels.base import DockPanel
from adx.panels.browser import BrowserPanel
from adx.panels.channel_rack.panel import ChannelRackPanel
from adx.panels.effect.panel import EffectPanel
from adx.panels.geometry_view import GeometryView
from adx.panels.history.panel import HistoryPanel
from adx.panels.inspector.panel import InspectorPanel
from adx.panels.instrument.panel import InstrumentPanel
from adx.panels.mixer.panel import MixerPanel
from adx.panels.piano_roll.panel import PianoRollPanel
from adx.panels.playlist.panel import PlaylistPanel
from adx.panels.waveform.widget import WaveformPanel
from adx.preferences import Preferences
from adx.selection import Context, Kind, Selection
from adx.session import Session
from adx.shortcuts import Keymap
from adx.theme.tokens import THEMES, Theme, stylesheet
from adx.transport_bar import TransportBar

#: Bump when the dock layout changes shape, so an old saved layout is not restored
#: into panels it does not describe. 2: Phase 6's panels.
LAYOUT_VERSION = 2

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


class MainWindow(QMainWindow):
    """The application window."""

    def __init__(self, engine: Engine, settings: QSettings | None = None) -> None:
        super().__init__()
        self.engine = engine
        self.settings = settings or QSettings("adX", "adX")
        self.prefs = Preferences.load(self.settings)
        self.session = Session(engine, self)
        self.selection = Selection(self)
        self.ops = EditOps(self.session, self.selection)
        self.path: str | None = None
        self.saved_revision = 0
        self.theme: Theme = THEMES["dark"]
        self.setWindowTitle("adX")
        self.setDockNestingEnabled(True)
        self.transport = TransportBar(self)
        self.addToolBar(self.transport)
        self._build_panels()
        self.resize(1600, 960)
        self._place_panels()
        self._needs_default_sizes = True
        self.keymap = Keymap(self.settings)
        self.registry = ActionRegistry(self.keymap, self)
        register_actions(self, self.registry)
        self.menus = self.registry.build_menus(self.menuBar())
        self._connect()
        self.clock = FrameClock(engine, parent=self)
        self.mixer.attach(self.clock)
        self.clock.frame.connect(self._on_frame)
        self.autosave = Autosave(
            self.session,
            lambda: self.path,
            lambda: self.prefs.project_dir or str(Path.cwd()),
            self.prefs.autosave_minutes,
            self,
        )
        self.statusBar().showMessage("Ready")
        engine_bridge.errors.error.connect(
            lambda where, message: self.status(f"{where}: {message}")
        )
        self.restore_layout()
        self.clock.start()

    # --- construction ---------------------------------------------------------------

    def _build_panels(self) -> None:
        s, sel = self.session, self.selection
        self.browser = BrowserPanel(str(Path.cwd()), self, session=s)
        self.channel_rack = ChannelRackPanel(s, sel, self)
        self.piano_roll = PianoRollPanel(self)
        self.playlist = PlaylistPanel(s, sel, self)
        self.mixer = MixerPanel(s, sel, self)
        self.instrument = InstrumentPanel(s, sel, self)
        self.effect = EffectPanel(s, sel, self)
        self.automation = AutomationPanel(s, sel, self)
        self.history = HistoryPanel(s, self)
        self.inspector = InspectorPanel(s, sel, self)
        self.waveform = WaveformPanel(self)
        self.panels: list[DockPanel] = [
            self.browser,
            self.channel_rack,
            self.history,
            self.playlist,
            self.piano_roll,
            self.mixer,
            self.automation,
            self.waveform,
            self.instrument,
            self.effect,
            self.inspector,
        ]

    def _connect(self) -> None:
        self.transport.play_requested.connect(self.toggle_play)
        self.transport.stop_requested.connect(self.stop)
        self.transport.loop_toggled.connect(self._loop)
        self.transport.tempo_changed.connect(self._tempo)
        self.transport.metronome_toggled.connect(self._metronome)
        self.browser.project_activated.connect(self.open_project)
        self.browser.audio_activated.connect(self.waveform.waveform.open)
        self.piano_roll.seek = self.engine.seek
        self.piano_roll.on_selection = self._roll_selection
        self.channel_rack.open_roll.connect(self._open_roll)
        self.channel_rack.open_instrument.connect(self._open_instrument)
        self.playlist.open_pattern.connect(self._open_pattern)
        self.mixer.open_effect.connect(self._open_effect)
        self.session.changed.connect(self._changed)
        self.session.failed.connect(lambda what, why: self.status(f"{what}: {why}"))
        self.session.after_change = self._update_title
        self.selection.changed.connect(self.registry.update_enabled)
        # A press starts a new gesture, so a new history entry, however quickly it
        # follows the last: the hot surfaces say so themselves (their input arrives
        # through QML), and every other widget's press is seen by the filter below.
        for view in self.findChildren(GeometryView):
            view.gesture_started.connect(self.session.seal)
        self.piano_roll.gesture_started.connect(self.session.seal)
        self.piano_roll.audition.connect(self._audition)
        app = QApplication.instance()
        if app is not None:
            app.installEventFilter(self)

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802 - Qt's name
        """Every press in the window starts a gesture, so a new history entry: a mouse
        press, or a key press that is not auto-repeat (holding a nudge key is one step).
        Within a gesture, edits coalesce however slowly the frames come."""
        kind = event.type()
        if kind == QEvent.Type.MouseButtonPress or (
            kind == QEvent.Type.KeyPress
            and isinstance(event, QKeyEvent)
            and not event.isAutoRepeat()
        ):
            self.session.seal()
        return super().eventFilter(watched, event)

    # --- the project ----------------------------------------------------------------

    @property
    def project(self) -> Project | None:
        """The open project."""
        return self.session.project

    def _adopt(self, project: Project, path: str | None) -> None:
        self.path = path
        self.saved_revision = project.revision()
        self.selection.clear()
        self.session.open(project)
        self.piano_roll.set_project(project, on_edit=self.session.notify)
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
            self.status(
                f"{Path(path).name}: {len(errors)} error(s); first at line "
                f"{first['line']}: {first['message']}"
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

    def export_wav(self, path: str) -> dict[str, object]:
        """Render the arrangement offline to a 32-bit float WAV (the full export dialog is
        Phase 8's). Returns the render statistics, the hash among them."""
        project = self.session.require()
        stats = engine_bridge.render_to_file(project, path)
        seconds = float(stats["frames"]) / float(stats["sample_rate"])
        self.status(f"Exported {Path(path).name}: {seconds:.1f} s")
        return stats

    def export_dialog(self) -> None:
        """Ask for a file, then export."""
        stem = Path(self.path).stem if self.path else "untitled"
        path, _filter = QFileDialog.getSaveFileName(
            self, "Export WAV", f"{stem}.wav", "WAV (*.wav)"
        )
        if path:
            self.export_wav(path)

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
        """A command was executed outside the session (kept for older callers)."""
        self.session.notify()

    def _changed(self) -> None:
        self.piano_roll.refresh()
        self.registry.update_enabled()

    def undo(self) -> None:
        """Undo one step, everywhere."""
        self.session.undo()

    def redo(self) -> None:
        """Redo one step, everywhere."""
        self.session.redo()

    def status(self, message: str) -> None:
        """A line in the status bar."""
        self.statusBar().showMessage(message, 8000)

    # --- cross-panel navigation -----------------------------------------------------

    def _open_roll(self, pattern: str, channel: str) -> None:
        if self.project is not None and pattern and channel:
            self.piano_roll.select_clip(pattern, channel)

    def _open_pattern(self, pattern: int) -> None:
        if self.project is None:
            return
        name = next(
            (str(p["name"]) for p in self.project.patterns_info() if int(p["id"]) == pattern), ""
        )
        if name:
            self.channel_rack.select_pattern(name)
            channels = self.project.pattern_channels(name) or self.project.channels()
            if channels:
                self.piano_roll.select_clip(name, channels[0])
            self.piano_roll.raise_()

    def _open_instrument(self, channel: int) -> None:
        self.instrument.show_channel(channel)
        self.instrument.show()
        self.instrument.raise_()

    def _open_effect(self, insert: int, slot: int) -> None:
        self.effect.show_slot(insert, slot)
        self.effect.show()
        self.effect.raise_()

    def _roll_selection(self) -> None:
        model = self.piano_roll.model
        if model is None:
            return
        ids = [int(i) for i in model.selection]
        self.selection.select(
            Kind.NOTES, ids, Context(pattern=model.pattern, channel=model.channel)
        )

    def _audition(self, channel: str, pitch: int, velocity: int) -> None:
        """A note through a channel's instrument, now: the engine's audition voice, no
        snapshot rebuild, heard whether or not the transport rolls (P5-1)."""
        if self.project is None:
            return
        ids = {str(c["name"]): int(c["id"]) for c in self.project.channels_info()}
        if channel in ids:
            self.engine.audition(ids[channel], pitch, velocity, 0.3)

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

    def toggle_loop(self) -> None:
        """Loop on or off (the transport bar's button follows)."""
        self.transport.loop_action.toggle()

    def _loop(self, enabled: bool) -> None:
        if self.project is not None:
            length = self.project.content_length()
            self.engine.set_loop(0, max(length, PPQ * 4), enabled)

    def toggle_metronome(self) -> None:
        """The click on or off."""
        self.transport.metronome_action.toggle()

    def _metronome(self, enabled: bool) -> None:
        numerator, denominator = self.project.meter_at(0) if self.project is not None else (4, 4)
        self.engine.set_metronome(enabled, PPQ * 4 // max(1, denominator), numerator)

    def toggle_follow(self) -> None:
        """Scroll the playlist with the playhead."""
        self.playlist.follow = not self.playlist.follow

    def add_marker_at_playhead(self) -> None:
        """A marker at the playhead, snapped to the beat."""
        project = self.session.require()
        tick = round(self.clock.position_ticks / PPQ) * PPQ
        self.session.execute(
            engine_bridge.commands.add_marker(tick, f"Marker {len(project.markers()) + 1}")
        )

    def _tempo(self, bpm: float) -> None:
        if self.project is not None:
            self.session.execute(engine_bridge.commands.set_tempo(bpm, 0))

    def _on_frame(self, ticks: int, rolling: bool) -> None:
        """The 60 Hz frame. The clock made the one engine call; this only draws."""
        self.transport.show_position(ticks, self.clock.position_seconds)
        self.transport.set_playing(rolling)
        self.piano_roll.frame(ticks, rolling)
        self.playlist.frame(ticks, rolling)
        self.mixer.frame(self.clock)

    # --- preferences, layout and theme ----------------------------------------------

    def preferences_dialog(self) -> None:
        """Edit the preferences; apply what can be applied now."""
        from adx.panels.preferences.dialog import PreferencesDialog

        latency = self.engine.latency_samples() / max(1, self.engine.sample_rate) * 1000.0
        dialog = PreferencesDialog(self.prefs, latency, self)
        if dialog.exec() and dialog.result_preferences is not None:
            self.apply_preferences(dialog.result_preferences)

    def apply_preferences(self, prefs: Preferences) -> None:
        """Store and apply preferences: theme, autosave, preset folders."""
        self.prefs = prefs
        prefs.save(self.settings)
        self.set_theme_name(prefs.theme)
        self.autosave.set_interval(prefs.autosave_minutes)
        self.browser.preset_dirs = [self.browser.preset_dirs[0], *prefs.preset_dirs]
        self.browser.refresh()

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
        self.set_theme(
            THEMES.get(str(self.settings.value("theme", self.prefs.theme)), THEMES["dark"])
        )

    def save_layout(self) -> None:
        """Persist the window, docks and panel state."""
        self.settings.setValue("window/geometry", self.saveGeometry())
        self.settings.setValue("window/state", self.saveState(LAYOUT_VERSION))
        for panel in self.panels:
            panel.save_state(self.settings)
        self.settings.setValue("theme", self.theme.name)

    def _place_panels(self) -> None:
        """The default arrangement: browser and rack left, playlist and piano roll in the
        middle, editors right, mixer and automation along the bottom."""
        for panel in self.panels:
            panel.setFloating(False)
            self.addDockWidget(panel.default_area, panel)
            panel.show()
        self.tabifyDockWidget(self.browser, self.channel_rack)
        self.tabifyDockWidget(self.channel_rack, self.history)
        self.tabifyDockWidget(self.playlist, self.piano_roll)
        self.tabifyDockWidget(self.instrument, self.effect)
        self.tabifyDockWidget(self.effect, self.inspector)
        self.tabifyDockWidget(self.mixer, self.automation)
        self.tabifyDockWidget(self.automation, self.waveform)
        self.channel_rack.raise_()
        self.playlist.raise_()
        self.instrument.raise_()
        self.mixer.raise_()

    def _size_panels(self) -> None:
        width, height = max(1000, self.width()), max(600, self.height())
        self.resizeDocks(
            [self.channel_rack, self.playlist, self.instrument],
            [int(width * 0.28), int(width * 0.5), int(width * 0.22)],
            Qt.Orientation.Horizontal,
        )
        self.resizeDocks(
            [self.playlist, self.mixer],
            [int(height * 0.58), int(height * 0.32)],
            Qt.Orientation.Vertical,
        )

    def showEvent(self, event: QShowEvent) -> None:  # noqa: N802 - Qt's name
        super().showEvent(event)
        if self._needs_default_sizes:
            self._needs_default_sizes = False
            QTimer.singleShot(0, self._size_panels)
        # Start on the arrangement, never in a text field (see TransportBar's tempo box).
        QTimer.singleShot(0, self.playlist.canvas.setFocus)

    def reset_layout(self) -> None:
        """Back to the default dock layout, every panel visible."""
        self._place_panels()
        self._size_panels()
        self.settings.remove("window/state")

    def set_theme_name(self, name: str) -> None:
        """Switch theme by name."""
        self.set_theme(THEMES.get(name, THEMES["dark"]))

    def set_theme(self, theme: Theme) -> None:
        """Switch theme: stylesheet, QML palettes and role colours. No geometry rebuilds."""
        self.theme = theme
        app = QApplication.instance()
        if isinstance(app, QApplication):
            app.setStyleSheet(stylesheet(theme))
        self.piano_roll.apply_theme(theme)
        self.waveform.waveform.apply_theme(theme)
        for panel in (self.playlist, self.channel_rack, self.mixer, self.automation, self.history):
            panel.apply_theme(theme)

    def about(self) -> None:
        """Version information."""
        engine = f"{engine_bridge.engine_version()} ({engine_bridge.git_sha()})"
        QMessageBox.about(self, "About adX", f"adX {__version__}\nengine {engine}")

    def closeEvent(self, event: QCloseEvent) -> None:  # noqa: N802 - Qt's name
        if not self._may_discard():
            event.ignore()
            return
        self.clock.stop()
        self.save_layout()
        app = QApplication.instance()
        if app is not None:
            app.removeEventFilter(self)
        event.accept()
