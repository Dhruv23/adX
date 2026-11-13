"""The main window's Phase 8 actions: export, MIDI files, collect-and-save, freeze and
recording (phase_8.md). Kept out of mainwindow.py, which is long enough already.

Each action reads the session and the selection, asks for a path where it needs one,
and reports in the status bar. Long work (an export) runs as a job polled from a timer;
the rest is quick enough to run in place.
"""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any

from PySide6.QtWidgets import QDialog, QFileDialog

from adx import engine_bridge
from adx.panels.export.dialog import ExportDialog, ExportRunner
from adx.panels.record.toolbar import RecordToolbar
from adx.selection import Kind

if TYPE_CHECKING:
    from adx.export_settings import ExportSettings
    from adx.mainwindow import MainWindow


class Delivery:
    """Phase 8's actions over one main window."""

    def __init__(self, window: MainWindow) -> None:
        self.window = window
        self.runner: ExportRunner | None = None
        #: The last export's result, for tests and the status line.
        self.last_export: dict[str, Any] | None = None
        self.record = RecordToolbar(
            window.engine,
            window.session,
            track=self.record_track,
            punch_range=self.punch_range,
            directory=self.project_directory,
            midi_channel=lambda: window.live_midi.target,
            parent=window,
        )
        self.record.message.connect(window.status)

    # --- where things go ------------------------------------------------------------

    def project_directory(self) -> str:
        """Beside the project file, else the preferences' project folder, else here."""
        w = self.window
        if w.path:
            return str(Path(w.path).parent)
        return w.prefs.project_dir or str(Path.cwd())

    def _selected_rows(self) -> list[Any]:
        project = self.window.session.project
        ids = set(self.window.selection.ids(Kind.CLIPS))
        if project is None or not ids:
            return []
        return [row for row in project.playlist_items() if int(row["item"]) in ids]

    def record_track(self) -> int | None:
        """The selected clip's track, else the last playlist track."""
        rows = self._selected_rows()
        if rows:
            return int(rows[0]["track"])
        project = self.window.session.project
        tracks = project.playlist_info()["tracks"] if project is not None else []
        return int(tracks[-1]["id"]) if tracks else None

    def punch_range(self) -> tuple[int, int] | None:
        """The span of the selected clips: punch records over exactly that."""
        rows = self._selected_rows()
        if not rows:
            return None
        start = min(int(row["start"]) for row in rows)
        end = max(int(row["start"]) + int(row["length"]) for row in rows)
        return (start, end) if end > start else None

    # --- export ---------------------------------------------------------------------

    def export_dialog(self) -> None:
        """The export matrix; OK starts the export in the background."""
        w = self.window
        project = w.session.require()
        stem = Path(w.path).stem if w.path else "untitled"
        output = str(Path(self.project_directory()) / f"{stem}.wav")
        markers = [name for _id, _tick, name in project.markers()]
        dialog = ExportDialog(output, markers, self.punch_range(), w)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            self.start_export(dialog.settings(), bundle=dialog.bundle.isChecked())

    def start_export(self, settings: ExportSettings, bundle: bool = False) -> ExportRunner:
        """Run ``settings`` as a job; the status bar follows its progress."""
        w = self.window
        if self.runner is not None:
            self.runner.cancel()
        # A render plays the project's plugin states: make them the live ones first.
        w.plugin_runtime.sync_states()
        runner = ExportRunner(w.session.require(), settings, w)
        runner.progress.connect(lambda done: w.status(f"Exporting… {done * 100:.0f}%"))
        runner.finished.connect(lambda result: self._exported(result, bundle))
        self.runner = runner
        return runner

    def _exported(self, result: dict[str, Any], bundle: bool) -> None:
        self.runner = None
        self.last_export = result
        w = self.window
        if result["cancelled"]:
            w.status("Export cancelled")
            return
        if not result["ok"]:
            w.status(f"Export failed: {result['error']}")
            return
        files = result["files"]
        first = Path(files[0]["path"]).name if files else ""
        more = f" and {len(files) - 1} more" if len(files) > 1 else ""
        w.status(f"Exported {first}{more}")
        if bundle:
            self.collect_and_save()

    # --- MIDI files -----------------------------------------------------------------

    def import_midi_dialog(self, gm_parts: bool = False) -> None:
        """Ask for a .mid (or a singing-voice project) and import it. With ``gm_parts``,
        a .mid lands on one 16-part General MIDI channel (Play on one GM channel)."""
        path, _filter = QFileDialog.getOpenFileName(
            self.window,
            "Import MIDI",
            self.project_directory(),
            "MIDI and vocal files (*.mid *.midi *.ust *.ustx *.vsqx);;MIDI files (*.mid *.midi);;"
            "UTAU, OpenUtau, Vocaloid (*.ust *.ustx *.vsqx)",
        )
        if path:
            self.import_midi(path, gm_parts=gm_parts)

    def import_midi(self, path: str, gm_parts: bool = False) -> dict[str, Any]:
        """A Standard MIDI File - or a UST, USTX or VSQX - into the project, one undo step.
        ``gm_parts`` puts a .mid's tracks on one 16-part ``gm`` channel, each MIDI channel
        a part with its programs, on the Windows GS bank until BANK= says otherwise."""
        w = self.window
        if Path(path).suffix.lower() in (".ust", ".ustx", ".vsqx"):
            from adx.sung_import import import_sung

            try:
                result = import_sung(w.session.require(), path)
            except (OSError, ValueError, RuntimeError) as error:
                w.status(f"Import: {error}")
                return {}
        else:
            result = engine_bridge.import_midi(w.session.require(), path, gm_parts=gm_parts)
        w.session.notify()
        w.status(f"Imported {Path(path).name}: {result['tracks']} track(s)")
        return result

    def export_midi_dialog(self) -> None:
        """Ask where, then write the arrangement as MIDI."""
        stem = Path(self.window.path).stem if self.window.path else "untitled"
        path, _filter = QFileDialog.getSaveFileName(
            self.window,
            "Export MIDI",
            str(Path(self.project_directory()) / f"{stem}.mid"),
            "MIDI files (*.mid)",
        )
        if path:
            engine_bridge.export_midi(self.window.session.require(), path)
            self.window.status(f"Wrote {Path(path).name}")

    # --- collect and save -----------------------------------------------------------

    def collect_and_save(self, destination: str | None = None) -> dict[str, Any] | None:
        """Save, then copy the project and every sample it uses into one folder and zip
        it. Without ``destination``, asks for the folder."""
        w = self.window
        if not w.path and not w.save_as():
            return None
        if not w.save():
            return None
        if destination is None:
            destination = QFileDialog.getExistingDirectory(
                w, "Collect into folder", str(Path(w.path or ".").parent)
            )
            if not destination:
                return None
            destination = str(Path(destination) / f"{Path(w.path or 'project').stem} bundle")
        result = engine_bridge.bundle_project(
            str(w.path), destination, zip_it=True, library=w.prefs.sample_dirs
        )
        if not result["ok"]:
            w.status(f"Collect failed: {result['error']}")
        elif result["missing"]:
            w.status(
                f"Collected {result['copied']} file(s); missing: {', '.join(result['missing'])}"
            )
        else:
            w.status(f"Collected {result['copied']} file(s) into {Path(result['zip']).name}")
        return result

    # --- freeze ---------------------------------------------------------------------

    def _channel(self) -> int | None:
        ids = self.window.selection.ids(Kind.CHANNELS)
        return ids[0] if ids else None

    def can_freeze(self) -> bool:
        """A channel is selected."""
        return self.window.session.project is not None and self._channel() is not None

    def freeze(self) -> None:
        """Render the selected channel to audio and play that instead (undoable)."""
        channel = self._channel()
        if channel is None:
            return
        w = self.window
        try:
            result = engine_bridge.freeze_channel(w.session.require(), channel)
        except RuntimeError as error:
            w.status(f"Freeze: {error}")
            return
        w.session.notify()
        w.status(f"Froze the channel to {Path(result['file']).name}")

    def unfreeze(self) -> None:
        """Put the selected channel's instrument and effects back."""
        channel = self._channel()
        if channel is None:
            return
        engine_bridge.unfreeze_channel(self.window.session.require(), channel)
        self.window.session.notify()
        self.window.status("Unfroze the channel")
