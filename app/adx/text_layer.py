"""The text-first layer in the window (phase_7.md): the editor panel, the console, the
file's sync, and the conflict question - wired to the shell in one place, so the main
window only says when a project opens, saves or plays.
"""

from __future__ import annotations

import difflib
from pathlib import Path
from typing import TYPE_CHECKING

from PySide6.QtCore import QObject
from PySide6.QtWidgets import QDialog, QDialogButtonBox, QMessageBox, QPlainTextEdit, QVBoxLayout

from adx.engine_bridge import Project
from adx.panels.editor.panel import EditorPanel
from adx.scripting import api
from adx.scripting.console import ConsolePanel
from adx.text_sync import MODES, FileSync

if TYPE_CHECKING:
    from adx.mainwindow import MainWindow


class TextLayer(QObject):
    """Owns the file sync, the editor and the console for one window."""

    def __init__(self, window: MainWindow) -> None:
        super().__init__(window)
        self.window = window
        self.file_sync = FileSync(window.session, self)
        self.editor = EditorPanel(window.session, self.file_sync, window)
        api.attach(window.session, window.engine)
        self.console = ConsolePanel({"adx": api}, window)
        #: The editor moved the piano roll's selection: do not move the editor back.
        self._from_editor = False
        self.file_sync.reloaded.connect(self._reloaded)
        self.file_sync.conflict.connect(self._conflict)
        self.file_sync.invalid.connect(
            lambda line, message: window.status(f"File not reloaded - line {line}: {message}")
        )
        self.file_sync.wrote.connect(self._file_written)
        self.editor.note_at_cursor.connect(self._select_in_roll)

    # --- the project's lifetime -------------------------------------------------------

    def opened(self, project: Project, path: str | None) -> None:
        """A project was opened (or created). Before the panels refresh."""
        text = None
        if path:
            try:
                text = Path(path).read_bytes().decode("utf-8")
            except (OSError, UnicodeDecodeError):
                text = None
        self.file_sync.attach(project, path, text)

    def saved(self, path: str) -> None:
        """The window wrote the project to ``path``."""
        try:
            text = Path(path).read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            return
        self.file_sync.saved(path, text)

    def set_mode(self, mode: str) -> None:
        """Sync mode from the preferences."""
        if mode in MODES:
            self.file_sync.set_mode(mode)

    def frame(self, ticks: int, rolling: bool) -> None:
        """The 60 Hz frame (no engine call)."""
        self.editor.frame(ticks, rolling)

    # --- the file changed ---------------------------------------------------------------

    def _reloaded(self, commands: int, structural: bool) -> None:
        window = self.window
        if window.project is not None:
            # The file and the project agree again.
            window.saved_revision = window.project.revision()
            window._update_title()
        kind = "structural" if structural else f"{commands} change(s)"
        window.status(f"Reloaded from disk: {kind}")

    def _file_written(self) -> None:
        window = self.window
        if window.project is not None:
            window.saved_revision = window.project.revision()
            window._update_title()

    def _conflict(self, theirs: str) -> None:
        """The file changed under unsaved edits: ask, never guess (phase_7.md 4.4)."""
        while True:
            box = QMessageBox(self.window)
            box.setWindowTitle("adX - the file changed on disk")
            box.setText(
                "The project file was changed by another program, and this window has "
                "changes that are not in it."
            )
            mine = box.addButton("Keep mine", QMessageBox.ButtonRole.AcceptRole)
            theirs_button = box.addButton("Take theirs", QMessageBox.ButtonRole.DestructiveRole)
            diff = box.addButton("Show diff", QMessageBox.ButtonRole.HelpRole)
            box.exec()
            clicked = box.clickedButton()
            if clicked is diff:
                self.show_diff(theirs)
                continue
            if clicked is theirs_button:
                self.file_sync.take_theirs()
            elif clicked is mine:
                self.file_sync.keep_mine()
            return

    def diff_text(self, theirs: str) -> str:
        """A unified diff from this window's project to the file's text."""
        sync = self.file_sync.sync
        mine = sync.render_text() if sync is not None else ""
        return "".join(
            difflib.unified_diff(
                mine.splitlines(keepends=True),
                theirs.splitlines(keepends=True),
                "this window",
                "the file on disk",
            )
        )

    def show_diff(self, theirs: str) -> None:
        """The diff, in a dialog."""
        dialog = QDialog(self.window)
        dialog.setWindowTitle("adX - this window vs the file")
        layout = QVBoxLayout(dialog)
        view = QPlainTextEdit(self.diff_text(theirs) or "(no textual difference)")
        view.setReadOnly(True)
        layout.addWidget(view)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(dialog.reject)
        layout.addWidget(buttons)
        dialog.resize(900, 600)
        dialog.exec()

    # --- the piano roll and the text, linked ------------------------------------------

    def roll_selected(self, pattern: str, channel: str, ids: list[int]) -> None:
        """The piano roll's selection changed: show the first selected note's line."""
        if self._from_editor or not ids:
            return
        self.editor.reveal_note(pattern, channel, ids[0])

    def _select_in_roll(self, pattern: str, channel: str, index: int) -> None:
        window = self.window
        project = window.project
        if project is None or pattern not in project.patterns():
            return
        notes = project.notes(pattern, channel)
        order = sorted(
            range(len(notes)),
            key=lambda i: (int(notes["start"][i]), int(notes["pitch"][i]), int(notes["id"][i])),
        )
        if not 0 <= index < len(order):
            return
        note = int(notes["id"][order[index]])
        roll = window.piano_roll
        if roll.model is None or roll.model.pattern != pattern or roll.model.channel != channel:
            roll.select_clip(pattern, channel)
        if roll.model is None:
            return
        self._from_editor = True
        try:
            roll.model.select([note])
            roll.selection_changed()
        finally:
            self._from_editor = False
