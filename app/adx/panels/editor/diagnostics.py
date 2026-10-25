"""Inline diagnostics: squiggles with hover text, gutter marks and a problem list that
navigates on click (phase_7.md 4.6).

The diagnostics are the engine's (IncrementalParse, engine/format/adx/Incremental.h):
the same codes, spans and messages ``adx validate`` prints, kept current keystroke by
keystroke. A span is a 1-based line and column in UTF-8 bytes; it is drawn at the
document's UTF-16 position (``to_utf16``), and a zero-length span (a missing ``]`` at
the end of a line, say) is drawn one character wide so it can be seen.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QBrush, QColor, QTextCharFormat, QTextCursor
from PySide6.QtWidgets import QListWidget, QListWidgetItem, QPlainTextEdit, QTextEdit, QWidget

from adx.panels.editor.highlighter import to_utf16
from adx.theme.tokens import DARK, Theme


@dataclass(frozen=True)
class Problem:
    """One diagnostic, where it is and what it says."""

    line: int
    column: int
    length: int
    severity: str
    code: str
    message: str
    hint: str = ""

    @staticmethod
    def of(entry: dict[str, Any]) -> Problem:
        """From the engine's dict."""
        return Problem(
            line=int(entry["line"]),
            column=int(entry["column"]),
            length=int(entry["length"]),
            severity=str(entry["severity"]),
            code=str(entry["code"]),
            message=str(entry["message"]),
            hint=str(entry.get("hint", "")),
        )

    def text(self) -> str:
        """As the problem list shows it."""
        where = f"{self.line}:{self.column}" if self.line > 0 else "-"
        hint = f" ({self.hint})" if self.hint else ""
        return f"{self.severity}  {self.code}  {where}  {self.message}{hint}"


def problem_range(editor: QPlainTextEdit, problem: Problem) -> tuple[int, int] | None:
    """Document positions (start, end) a problem underlines, or None when it has no line."""
    if problem.line < 1:
        return None
    block = editor.document().findBlockByNumber(problem.line - 1)
    if not block.isValid():
        return None
    text = block.text()
    start, length = to_utf16(text, max(0, problem.column - 1), problem.length)
    start = min(start, len(text))
    if length == 0:
        # Nothing to underline (a missing character at the end): mark one character,
        # the last one if the span is past it.
        if start >= len(text) and start > 0:
            start -= 1
        length = 1 if text else 0
    return block.position() + start, block.position() + min(start + length, max(len(text), 1))


def squiggles(
    editor: QPlainTextEdit, problems: list[Problem], theme: Theme
) -> list[QTextEdit.ExtraSelection]:
    """A wave underline per problem, coloured by severity."""
    out: list[QTextEdit.ExtraSelection] = []
    for problem in problems:
        span = problem_range(editor, problem)
        if span is None:
            continue
        selection = QTextEdit.ExtraSelection()
        fmt = QTextCharFormat()
        fmt.setUnderlineStyle(QTextCharFormat.UnderlineStyle.WaveUnderline)
        fmt.setUnderlineColor(QColor(theme.syntax.get(f"diag-{problem.severity}", theme.danger)))
        fmt.setToolTip(f"{problem.code}: {problem.message}")
        selection.format = fmt
        cursor = QTextCursor(editor.document())
        cursor.setPosition(span[0])
        cursor.setPosition(span[1], QTextCursor.MoveMode.KeepAnchor)
        selection.cursor = cursor
        out.append(selection)
    return out


def problems_at(editor: QPlainTextEdit, problems: list[Problem], position: int) -> list[Problem]:
    """The problems whose underline covers document ``position`` (hover text)."""
    found = []
    for problem in problems:
        span = problem_range(editor, problem)
        if span is not None and span[0] <= position < max(span[1], span[0] + 1):
            found.append(problem)
    return found


class ProblemList(QListWidget):
    """Every problem in the file; a click goes to it."""

    #: (line, column), 1-based.
    activated = Signal(int, int)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.problems: list[Problem] = []
        self._theme: Theme = DARK
        self.itemClicked.connect(self._clicked)
        self.itemActivated.connect(self._clicked)

    def set_problems(self, problems: list[Problem]) -> None:
        """Show ``problems``, errors first."""
        order = {"error": 0, "warning": 1, "info": 2}
        self.problems = sorted(problems, key=lambda p: (order.get(p.severity, 3), p.line, p.column))
        self.clear()
        for problem in self.problems:
            item = QListWidgetItem(problem.text())
            item.setData(Qt.ItemDataRole.UserRole, (problem.line, problem.column))
            item.setForeground(
                QBrush(QColor(self._theme.syntax.get(f"diag-{problem.severity}", self._theme.text)))
            )
            self.addItem(item)

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.set_problems(self.problems)

    def _clicked(self, item: QListWidgetItem) -> None:
        line, column = item.data(Qt.ItemDataRole.UserRole)
        self.activated.emit(int(line), int(column))
