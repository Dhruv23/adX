"""The editor panel: the project as text, edited live (phase_7.md 4.6).

The text in this panel and the project are two views of one thing, and they stay in
step through commands (engine/sync/TextSync.h):

- **typing** re-runs the diagnostics at once (IncrementalParse - squiggles, gutter, the
  problem list) and, when the text parses cleanly, applies it after a short pause: the
  text is diffed into the narrowest commands and executed, so the piano roll, the mixer
  and the sound follow - and Undo undoes it like any other edit. Text with errors is
  never applied; the project keeps what it had until the text says something whole.
- **an edit anywhere else** - a panel, a script, the file changing on disk - replaces the
  text with the project's, keeping the cursor where it was. If the text has edits of its
  own not yet applied, it is left alone and a bar says so, offering the project's text.
- **while playing**, the mini-notation step that is sounding is lit (stepmarks.py).

The cursor is linked both ways with the piano roll: a note selected there brings its
line into view here, and the cursor on a note line here selects that note there.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSplitter,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from adx.engine_bridge import IncrementalParse, Project
from adx.panels.base import DockPanel
from adx.panels.editor.code_editor import CodeEditor
from adx.panels.editor.completion import Completions
from adx.panels.editor.diagnostics import Problem, ProblemList, squiggles
from adx.panels.editor.highlighter import EngineHighlighter
from adx.panels.editor.stepmarks import StepMarks, find_blocks, strip_comment
from adx.session import Session
from adx.text_sync import FileSync
from adx.theme.tokens import DARK, Theme

#: How long typing pauses before the text is applied.
APPLY_DELAY_MS = 150
#: How long edits elsewhere settle before the text is re-rendered: a drag is many edits,
#: and re-rendering a long file on each would make the drag stutter.
REFRESH_DELAY_MS = 120


def section_at(lines: list[str], line: int) -> tuple[str, int]:
    """The header of the section containing 0-based ``line`` and that header's index
    (("", -1) before the first one)."""
    for index in range(min(line, len(lines) - 1), -1, -1):
        if lines[index].startswith("["):
            return lines[index].strip(), index
    return "", -1


def section_type(header: str) -> str:
    """``[PATTERN Verse]`` -> ``PATTERN``."""
    inner = header.strip("[] ")
    return inner.split(" ", 1)[0] if inner else ""


class EditorPanel(DockPanel):
    """The project's .adx text, edited in place."""

    panel_id = "editor"
    title = "Text"
    default_area = Qt.DockWidgetArea.RightDockWidgetArea

    #: The cursor is on a note line: (pattern, channel, index in the clip's sorted notes).
    note_at_cursor = Signal(str, str, int)

    def __init__(
        self, session: Session, file_sync: FileSync, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.file_sync = file_sync
        self.theme: Theme = DARK
        self.editor = CodeEditor()
        self.highlighter = EngineHighlighter(self.editor.document())
        self.incremental = IncrementalParse()
        self.steps = StepMarks()
        self.completions = Completions()
        self.problems: list[Problem] = []
        self.problem_list = ProblemList()
        self.problem_list.activated.connect(self.editor.go_to)
        self.editor.candidates = self._candidates
        #: The project revision the text was last in step with.
        self.synced_revision = -1
        #: The text has edits of its own not yet applied.
        self.buffer_dirty = False
        self._loading = False
        self._applying = False
        #: The text the panel last reacted to.
        self._text_seen = ""
        self._lit = False
        self._build()
        self._apply_timer = QTimer(self)
        self._apply_timer.setSingleShot(True)
        self._apply_timer.setInterval(APPLY_DELAY_MS)
        self._apply_timer.timeout.connect(self.apply_now)
        self._refresh_timer = QTimer(self)
        self._refresh_timer.setSingleShot(True)
        self._refresh_timer.setInterval(REFRESH_DELAY_MS)
        self._refresh_timer.timeout.connect(self._refresh_if_stale)
        #: The project changed while the text was not being shown.
        self._stale = False
        self.visibilityChanged.connect(self._visibility_changed)
        self.editor.textChanged.connect(self._text_changed)
        self.editor.cursorPositionChanged.connect(self._cursor_moved)
        session.changed.connect(self._project_changed)
        session.opened.connect(self.load_from_project)

    def _build(self) -> None:
        body = QWidget()
        layout = QVBoxLayout(body)
        layout.setContentsMargins(0, 0, 0, 0)
        # A tool bar, not a row of widgets: it folds into its extension menu when the panel
        # is narrow, so the panel never pins the splitter beside it.
        bar = QToolBar()
        self.live = QCheckBox("Apply as you type")
        self.live.setChecked(True)
        self.live.setToolTip("Apply the text to the project whenever it parses cleanly")
        bar.addWidget(self.live)
        bar.addAction("Apply", self.apply_now)
        revert = bar.addAction("Revert section", self.revert_section)
        revert.setToolTip("Put the section under the cursor back as the project has it")
        self.status = QLabel("")
        bar.addWidget(self.status)
        bar.setMinimumWidth(60)
        layout.addWidget(bar)
        self.banner = QFrame()
        banner = QHBoxLayout(self.banner)
        banner.setContentsMargins(6, 2, 6, 2)
        self.banner_label = QLabel(
            "The project changed elsewhere; this text has edits not applied yet."
        )
        load = QPushButton("Load project text")
        load.clicked.connect(self.load_from_project)
        banner.addWidget(self.banner_label, 1)
        banner.addWidget(load)
        self.banner.hide()
        layout.addWidget(self.banner)
        split = QSplitter(Qt.Orientation.Vertical)
        self.editor.setMinimumSize(60, 30)
        self.problem_list.setMinimumSize(60, 0)
        split.addWidget(self.editor)
        split.addWidget(self.problem_list)
        split.setMinimumSize(60, 40)
        split.setStretchFactor(0, 4)
        split.setStretchFactor(1, 1)
        layout.addWidget(split, 1)
        self.setWidget(body)

    # --- the text <-> the project ------------------------------------------------------

    def text(self) -> str:
        """The editor's text."""
        return self.editor.toPlainText()

    def set_text(self, text: str) -> None:
        """Replace the text, keeping the cursor's line and column and the scroll."""
        cursor = self.editor.textCursor()
        line, column = cursor.blockNumber(), cursor.positionInBlock()
        scroll = self.editor.verticalScrollBar().value()
        self._loading = True
        try:
            self.editor.setPlainText(text)
            self.editor.go_to(line + 1, column + 1, focus=False)
            self.editor.verticalScrollBar().setValue(scroll)
        finally:
            self._loading = False
            self._text_seen = self.text()
        self._diagnose(full=True)

    def load_from_project(self) -> None:
        """Show the project's text (on open, or when asked)."""
        project = self.session.project
        sync = self.file_sync.sync
        if project is None or sync is None:
            return
        text = sync.render_text()
        if text != self.text():
            self.set_text(text)
        self.synced_revision = project.revision()
        self.buffer_dirty = False
        self._stale = False
        self.banner.hide()
        self.rebuild_steps()

    def _project_changed(self) -> None:
        project = self.session.project
        if self._applying or project is None or project.revision() == self.synced_revision:
            return
        if self.buffer_dirty:
            self.banner.show()
            return
        self._stale = True
        if self.isVisible() and not self.visibleRegion().isEmpty():
            self._refresh_timer.start()

    def _visibility_changed(self, shown: bool) -> None:
        if shown:
            self._refresh_if_stale()

    def _refresh_if_stale(self) -> None:
        """Re-render the text if the project moved on while it was hidden or settling."""
        if self._stale and not self.buffer_dirty:
            self.load_from_project()

    def _text_changed(self) -> None:
        # textChanged also fires when only formatting changes - which the highlighter's
        # first, deferred pass does after every load - and the document's revision moves
        # then too. So compare the text itself: a format is not an edit.
        text = self.text()
        if self._loading or text == self._text_seen:
            self._text_seen = text
            return
        self._text_seen = text
        self.buffer_dirty = True
        self._diagnose()
        if self.live.isChecked():
            self._apply_timer.start()

    def _diagnose(self, full: bool = False) -> None:
        text = self.text()
        if full:
            self.incremental.reset(text)
            entries = self.incremental.update(text)
        else:
            entries = self.incremental.update(text)
        self.problems = [Problem.of(entry) for entry in entries]
        self.editor.set_problems(self.problems, squiggles(self.editor, self.problems, self.theme))
        self.problem_list.set_problems(self.problems)
        errors = sum(1 for p in self.problems if p.severity == "error")
        self.status.setText(f"{errors} error(s)" if errors else "")

    def apply_now(self) -> bool:
        """Apply the text to the project, if it parses cleanly. True when it changed."""
        self._apply_timer.stop()
        sync = self.file_sync.sync
        project = self.session.project
        if sync is None or project is None or not self.buffer_dirty:
            return False
        if any(p.severity == "error" for p in self.problems):
            self.status.setText("not applied: the text has errors")
            return False
        self._applying = True
        try:
            result = sync.apply_text(self.text())
            status = str(result["status"])
            if status == "applied":
                self.session.notify()
        finally:
            self._applying = False
        if status in ("applied", "unchanged"):
            self.synced_revision = project.revision()
            self.buffer_dirty = False
            self.banner.hide()
            self.rebuild_steps()
            if status == "applied":
                self.status.setText(f"applied ({result['commands']} command(s))")
            return status == "applied"
        self.status.setText(f"not applied: {status}")
        return False

    def revert_section(self) -> None:
        """Put the section under the cursor back as the project has it (undoing this
        text's changes in that section only)."""
        sync = self.file_sync.sync
        if sync is None:
            return
        mine = self.text().split("\n")
        theirs = sync.render_text().replace("\r\n", "\n").split("\n")
        header, start = section_at(mine, self.editor.textCursor().blockNumber())
        if start < 0:
            return
        end = next((i for i in range(start + 1, len(mine)) if mine[i].startswith("[")), len(mine))
        replacement: list[str] = []
        if header in (line.strip() for line in theirs):
            at = next(i for i, line in enumerate(theirs) if line.strip() == header)
            stop = next(
                (i for i in range(at + 1, len(theirs)) if theirs[i].startswith("[")), len(theirs)
            )
            replacement = theirs[at:stop]
        self.set_text("\n".join(mine[:start] + replacement + mine[end:]))
        self.buffer_dirty = True
        if self.live.isChecked():
            self.apply_now()

    # --- playback ------------------------------------------------------------------------

    def rebuild_steps(self) -> None:
        """Re-read what each MINI block plays (after an edit, not per frame)."""
        project = self.session.project
        if project is not None:
            self.steps.rebuild(project, self.text())

    def frame(self, ticks: int, rolling: bool) -> None:
        """The 60 Hz frame: light the sounding steps (no engine call)."""
        if rolling and self.steps.blocks:
            self.editor.set_steps(self.steps.selections(self.editor, ticks, self.theme))
            self._lit = True
        elif self._lit:
            self.editor.set_steps([])
            self._lit = False

    # --- the cursor link with the piano roll -------------------------------------------

    def reveal_note(self, pattern: str, channel: str, note_id: int) -> bool:
        """Bring the line of note ``note_id`` into view (the piano roll selected it)."""
        project: Project | None = self.session.project
        if project is None or self.buffer_dirty:
            return False
        notes = project.notes(pattern, channel)
        order = sorted(
            range(len(notes)),
            key=lambda i: (int(notes["start"][i]), int(notes["pitch"][i]), int(notes["id"][i])),
        )
        ids = [int(notes["id"][i]) for i in order]
        if note_id not in ids:
            return False
        opener = self._notes_line(pattern, channel)
        if opener < 0:
            return False
        self.editor.blockSignals(True)
        self.editor.go_to(opener + 2 + ids.index(note_id), 3, focus=False)
        self.editor.blockSignals(False)
        self.editor.refresh_selections()
        return True

    def _notes_line(self, pattern: str, channel: str) -> int:
        """0-based line of ``NOTES channel`` in ``[PATTERN pattern]``, or -1."""
        lines = self.text().split("\n")
        in_pattern = False
        for number, raw in enumerate(lines):
            if raw.startswith("["):
                in_pattern = raw.strip() == f"[PATTERN {pattern}]"
                continue
            words = strip_comment(raw).split(None, 1)
            if (
                in_pattern
                and len(words) == 2
                and words[0] == "NOTES"
                and words[1].strip('"') == channel
            ):
                return number
        return -1

    def _cursor_moved(self) -> None:
        # Only a person moving the cursor here moves the piano roll: not a reload putting
        # the cursor back, and not the roll revealing a note.
        if self._loading or not self.editor.hasFocus():
            return
        self.link_cursor()

    def link_cursor(self) -> None:
        """If the cursor is on a note line, select that note in the piano roll."""
        cursor = self.editor.textCursor()
        lines = self.text().split("\n")
        line = cursor.blockNumber()
        header, _start = section_at(lines, line)
        if (
            section_type(header) != "PATTERN"
            or line >= len(lines)
            or not lines[line].startswith(" ")
        ):
            return
        opener = line - 1
        while opener >= 0 and lines[opener].startswith(" "):
            opener -= 1
        words = strip_comment(lines[opener]).split(None, 1) if opener >= 0 else []
        if len(words) == 2 and words[0] == "NOTES" and not self.buffer_dirty:
            pattern = header.strip("[]")[len("PATTERN ") :]
            self.note_at_cursor.emit(pattern, words[1].strip('"'), line - opener - 1)

    # --- completion ----------------------------------------------------------------------

    def _candidates(self) -> list[str]:
        lines = self.text().split("\n")
        line = self.editor.textCursor().blockNumber()
        header, _start = section_at(lines, line)
        current = (
            self.editor.textCursor().block().text()[: self.editor.textCursor().positionInBlock()]
        )
        in_mini = any(
            line in (number for number, _column, _content in block.lines)
            for block in find_blocks(self.text())
        )
        return self.completions.candidates(
            section_type(header),
            current,
            self.editor.word_under_cursor(),
            self.session.project,
            in_mini,
        )

    def apply_theme(self, theme: Theme) -> None:
        """Recolour everything."""
        self.theme = theme
        self.highlighter.set_theme(theme)
        self.editor.set_theme(theme)
        self.problem_list.set_theme(theme)
        self._diagnose(full=True)
