"""The text widget under the editor panel: a ``QPlainTextEdit`` with a line-number gutter
that marks lines with problems, hover text for squiggles, completion on Ctrl+Space, and
two-space indentation (the canonical writer's, docs/adx-format-v2.md 9).

No new dependency (phase_7.md 4.6): Qt's own plain-text editor, a QSyntaxHighlighter and
extra selections are enough.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import QRect, QSize, QStringListModel, Qt
from PySide6.QtGui import (
    QColor,
    QFontDatabase,
    QKeyEvent,
    QMouseEvent,
    QPainter,
    QPaintEvent,
    QResizeEvent,
    QTextCursor,
    QTextFormat,
)
from PySide6.QtWidgets import QCompleter, QPlainTextEdit, QTextEdit, QToolTip, QWidget

from adx.panels.editor.diagnostics import Problem, problems_at
from adx.theme.tokens import DARK, Theme

#: Characters a completable word is made of: names, keys and dotted parameter paths.
WORD_CHARS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.=#[")


class _Gutter(QWidget):
    """Line numbers, and a mark beside each line that has a problem."""

    def __init__(self, editor: CodeEditor) -> None:
        super().__init__(editor)
        self.editor = editor

    def sizeHint(self) -> QSize:  # noqa: N802 - Qt's name
        return QSize(self.editor.gutter_width(), 0)

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        self.editor.paint_gutter(event)


class CodeEditor(QPlainTextEdit):
    """A plain-text editor for .adx text."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        font = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
        font.setPointSize(max(font.pointSize(), 10))
        self.setFont(font)
        self.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self.setTabStopDistance(self.fontMetrics().horizontalAdvance(" ") * 2)
        self.theme: Theme = DARK
        self.problems: list[Problem] = []
        self.squiggles: list[QTextEdit.ExtraSelection] = []
        self.steps: list[QTextEdit.ExtraSelection] = []
        self.marks: dict[int, str] = {}
        self._gutter = _Gutter(self)
        self.blockCountChanged.connect(self._update_gutter_width)
        self.updateRequest.connect(self._update_gutter)
        self.cursorPositionChanged.connect(self.refresh_selections)
        self._update_gutter_width()
        self.setMouseTracking(True)
        self.completer = QCompleter(self)
        self.completer.setWidget(self)
        self.completer.setCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        self.completer.setCompletionMode(QCompleter.CompletionMode.PopupCompletion)
        self.completer.activated.connect(self.insert_completion)
        #: Returns candidates for the word under the cursor (set by the panel).
        self.candidates: Callable[[], list[str]] | None = None

    # --- the gutter -----------------------------------------------------------------

    def gutter_width(self) -> int:
        """Wide enough for the largest line number and a mark."""
        digits = len(str(max(1, self.blockCount())))
        return 14 + self.fontMetrics().horizontalAdvance("9") * digits

    def _update_gutter_width(self, _count: int = 0) -> None:
        self.setViewportMargins(self.gutter_width(), 0, 0, 0)

    def _update_gutter(self, rect: QRect, dy: int) -> None:
        if dy:
            self._gutter.scroll(0, dy)
        else:
            self._gutter.update(0, rect.y(), self._gutter.width(), rect.height())

    def resizeEvent(self, event: QResizeEvent) -> None:  # noqa: N802 - Qt's name
        super().resizeEvent(event)
        area = self.contentsRect()
        self._gutter.setGeometry(QRect(area.left(), area.top(), self.gutter_width(), area.height()))

    def paint_gutter(self, event: QPaintEvent) -> None:
        """Line numbers, and a dot in the severity's colour on a line with a problem."""
        painter = QPainter(self._gutter)
        painter.fillRect(event.rect(), QColor(self.theme.syntax.get("gutter", self.theme.panel)))
        block = self.firstVisibleBlock()
        top = round(self.blockBoundingGeometry(block).translated(self.contentOffset()).top())
        height = self.fontMetrics().height()
        painter.setPen(QColor(self.theme.syntax.get("gutter-text", self.theme.text_dim)))
        while block.isValid() and top <= event.rect().bottom():
            bottom = top + round(self.blockBoundingRect(block).height())
            if block.isVisible() and bottom >= event.rect().top():
                number = block.blockNumber() + 1
                painter.drawText(
                    0,
                    top,
                    self._gutter.width() - 10,
                    height,
                    Qt.AlignmentFlag.AlignRight,
                    str(number),
                )
                severity = self.marks.get(number)
                if severity is not None:
                    colour = QColor(self.theme.syntax.get(f"diag-{severity}", self.theme.danger))
                    painter.setBrush(colour)
                    painter.setPen(Qt.PenStyle.NoPen)
                    painter.drawEllipse(self._gutter.width() - 8, top + height // 2 - 3, 6, 6)
                    painter.setPen(
                        QColor(self.theme.syntax.get("gutter-text", self.theme.text_dim))
                    )
            block = block.next()
            top = bottom

    # --- selections -----------------------------------------------------------------

    def set_problems(
        self, problems: list[Problem], squiggles: list[QTextEdit.ExtraSelection]
    ) -> None:
        """Show these diagnostics: underlines and gutter marks."""
        self.problems = problems
        self.squiggles = squiggles
        order = {"error": 0, "warning": 1, "info": 2}
        marks: dict[int, str] = {}
        for problem in problems:
            if problem.line < 1:
                continue
            existing = marks.get(problem.line)
            if existing is None or order.get(problem.severity, 3) < order.get(existing, 3):
                marks[problem.line] = problem.severity
        self.marks = marks
        self._gutter.update()
        self.refresh_selections()

    def set_steps(self, steps: list[QTextEdit.ExtraSelection]) -> None:
        """Light the sounding mini-notation steps."""
        self.steps = steps
        self.refresh_selections()

    def refresh_selections(self) -> None:
        """The current line, the step marks, then the squiggles on top."""
        current = QTextEdit.ExtraSelection()
        current.format.setBackground(
            QColor(self.theme.syntax.get("current-line", self.theme.raised))
        )
        current.format.setProperty(QTextFormat.Property.FullWidthSelection, True)
        current.cursor = self.textCursor()
        current.cursor.clearSelection()
        self.setExtraSelections([current, *self.steps, *self.squiggles])

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self.theme = theme
        self._gutter.update()
        self.refresh_selections()

    # --- hover and completion -------------------------------------------------------

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        """Hover text for the problem under the pointer."""
        super().mouseMoveEvent(event)
        position = self.cursorForPosition(event.position().toPoint()).position()
        found = problems_at(self, self.problems, position)
        if found:
            QToolTip.showText(
                event.globalPosition().toPoint(),
                "\n".join(
                    f"{p.code}: {p.message}" + (f"\n  {p.hint}" if p.hint else "") for p in found
                ),
                self,
            )
        else:
            QToolTip.hideText()

    def word_under_cursor(self) -> str:
        """The completable word that ends at the cursor."""
        cursor = self.textCursor()
        text = cursor.block().text()[: cursor.positionInBlock()]
        start = len(text)
        while start > 0 and text[start - 1] in WORD_CHARS:
            start -= 1
        return text[start:]

    def complete(self) -> bool:
        """Offer completions for the word at the cursor. False when there are none."""
        if self.candidates is None:
            return False
        words = self.candidates()
        popup = self.completer.popup()
        if popup is None:
            return False
        if not words:
            popup.hide()
            return False
        self.completer.setModel(QStringListModel(words, self.completer))
        self.completer.setCompletionPrefix(self.word_under_cursor())
        rect = self.cursorRect()
        bar = popup.verticalScrollBar()
        rect.setWidth(popup.sizeHintForColumn(0) + (bar.sizeHint().width() if bar else 0))
        self.completer.complete(rect)
        return True

    def insert_completion(self, word: str) -> None:
        """Replace the word at the cursor with ``word``."""
        cursor = self.textCursor()
        prefix = self.word_under_cursor()
        cursor.movePosition(
            QTextCursor.MoveOperation.Left, QTextCursor.MoveMode.KeepAnchor, len(prefix)
        )
        cursor.insertText(word)
        self.setTextCursor(cursor)

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802 - Qt's name
        """Completion keys, and Tab as two spaces."""
        popup = self.completer.popup()
        if (
            popup is not None
            and popup.isVisible()
            and event.key()
            in (
                Qt.Key.Key_Enter,
                Qt.Key.Key_Return,
                Qt.Key.Key_Escape,
                Qt.Key.Key_Tab,
            )
        ):
            event.ignore()
            return
        if (
            event.key() == Qt.Key.Key_Space
            and event.modifiers() & Qt.KeyboardModifier.ControlModifier
        ):
            self.complete()
            return
        if event.key() == Qt.Key.Key_Tab and not event.modifiers():
            self.insertPlainText("  ")
            return
        super().keyPressEvent(event)
        if popup is not None and popup.isVisible():
            word = self.word_under_cursor()
            if word:
                self.completer.setCompletionPrefix(word)
            else:
                popup.hide()

    def go_to(self, line: int, column: int = 1, focus: bool = True) -> None:
        """Put the cursor at a 1-based line and column, and show it. ``focus`` False
        leaves the keyboard where it is (the piano roll revealing a note's line)."""
        block = self.document().findBlockByNumber(max(0, line - 1))
        if not block.isValid():
            return
        cursor = QTextCursor(block)
        cursor.movePosition(
            QTextCursor.MoveOperation.Right,
            QTextCursor.MoveMode.MoveAnchor,
            min(max(0, column - 1), len(block.text())),
        )
        self.setTextCursor(cursor)
        self.centerCursor()
        if focus:
            self.setFocus()
