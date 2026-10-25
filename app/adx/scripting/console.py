"""The console: a Python REPL in a dock, running in the app's own interpreter
(phase_7.md 4.7 rule 4).

It is a real one - ``code.InteractiveInterpreter`` underneath, so multi-line blocks,
``print`` and tracebacks behave as in a terminal - with history on Up/Down and completion
on Tab (``rlcompleter``, over the console's namespace). ``adx`` is the scripting module
(api.py), already attached to the open project: what a script does here is a command,
on the main thread, and one Undo away.
"""

from __future__ import annotations

import code
import contextlib
import rlcompleter
import sys
import traceback
from collections.abc import Callable
from pathlib import Path
from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QFontDatabase, QKeyEvent, QTextCursor
from PySide6.QtWidgets import QLineEdit, QPlainTextEdit, QVBoxLayout, QWidget

from adx.panels.base import DockPanel

PROMPT = ">>> "
MORE = "... "


class _Interpreter(code.InteractiveInterpreter):
    """Writes tracebacks to the console. Formats them itself: the base class hands them to
    ``sys.excepthook`` when that has been replaced - as the app replaces it - and a typo at
    the prompt is not an application error."""

    def __init__(self, namespace: dict[str, Any], sink: Callable[[str], None]) -> None:
        super().__init__(namespace)
        self._sink = sink

    def write(self, data: str) -> None:
        self._sink(data)

    def showtraceback(self) -> None:
        kind, value, tb = sys.exc_info()
        # Drop the interpreter's own frame: the user's code is what failed.
        self.write("".join(traceback.format_exception(kind, value, tb.tb_next if tb else None)))

    def showsyntaxerror(self, filename: str | None = None, **_kwargs: object) -> None:
        kind, value, _tb = sys.exc_info()
        if kind is not None and value is not None:
            self.write("".join(traceback.format_exception_only(kind, value)))


class _Writer:
    """A file-like object that appends to the console."""

    def __init__(self, sink: Callable[[str], None]) -> None:
        self._sink = sink

    def write(self, data: str) -> int:
        self._sink(data)
        return len(data)

    def flush(self) -> None:
        """Nothing is buffered."""


class _Input(QLineEdit):
    """The input line: Enter runs, Up/Down walk the history, Tab completes."""

    submitted = Signal(str)

    def __init__(self, console: ConsolePanel) -> None:
        super().__init__()
        self.console = console
        self.history: list[str] = []
        self._position = 0

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802 - Qt's name
        key = event.key()
        if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            line = self.text()
            if line.strip() and (not self.history or self.history[-1] != line):
                self.history.append(line)
            self._position = len(self.history)
            self.clear()
            self.submitted.emit(line)
            return
        if key == Qt.Key.Key_Up and self.history:
            self._position = max(0, self._position - 1)
            self.setText(self.history[self._position])
            return
        if key == Qt.Key.Key_Down and self.history:
            self._position = min(len(self.history), self._position + 1)
            self.setText(self.history[self._position] if self._position < len(self.history) else "")
            return
        if key == Qt.Key.Key_Tab:
            self.setText(self.console.complete(self.text()))
            return
        super().keyPressEvent(event)

    def focusNextPrevChild(self, _next: bool) -> bool:  # noqa: N802 - Qt's name
        """Tab completes; it does not move the focus."""
        return False


class ConsolePanel(DockPanel):
    """An interactive Python console with ``adx`` in scope."""

    panel_id = "console"
    title = "Console"
    default_area = Qt.DockWidgetArea.BottomDockWidgetArea

    def __init__(
        self, namespace: dict[str, Any] | None = None, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.namespace: dict[str, Any] = namespace if namespace is not None else {}
        self.namespace.setdefault("__name__", "__console__")
        self.interpreter = _Interpreter(self.namespace, self.write)
        self._completer = rlcompleter.Completer(self.namespace)
        self._buffer: list[str] = []
        body = QWidget()
        layout = QVBoxLayout(body)
        layout.setContentsMargins(0, 0, 0, 0)
        font = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
        self.output = QPlainTextEdit()
        self.output.setReadOnly(True)
        self.output.setFont(font)
        self.input = _Input(self)
        self.input.setFont(font)
        self.input.setPlaceholderText(">>> adx.project().channels()")
        self.input.submitted.connect(self.run)
        self.output.setMinimumSize(60, 30)
        layout.addWidget(self.output, 1)
        layout.addWidget(self.input)
        self.setWidget(body)
        self.write("adX console - `adx` is the scripting module (docs/scripting.md).\n")

    def write(self, text: str) -> None:
        """Append to the output."""
        cursor = self.output.textCursor()
        cursor.movePosition(QTextCursor.MoveOperation.End)
        cursor.insertText(text)
        self.output.setTextCursor(cursor)
        self.output.ensureCursorVisible()

    def run(self, line: str) -> bool:
        """Run one input line. True when the interpreter wants more lines (an open
        block)."""
        self.write(f"{MORE if self._buffer else PROMPT}{line}\n")
        self._buffer.append(line)
        source = "\n".join(self._buffer)
        writer = _Writer(self.write)
        with contextlib.redirect_stdout(writer), contextlib.redirect_stderr(writer):
            more = self.interpreter.runsource(source, "<console>")
        if not more:
            self._buffer.clear()
        self.input.setPlaceholderText(
            "" if not more else "(continue the block; empty line ends it)"
        )
        return bool(more)

    def run_file(self, path: str) -> None:
        """Run a script file in the console's namespace."""
        source = Path(path).read_text(encoding="utf-8")
        self.write(f"# running {path}\n")
        writer = _Writer(self.write)
        with contextlib.redirect_stdout(writer), contextlib.redirect_stderr(writer):
            self.interpreter.runcode(compile(source, path, "exec"))

    def complete(self, text: str) -> str:
        """Tab: complete the last word of ``text`` (all candidates are listed when there
        is more than one)."""
        start = len(text)
        while start > 0 and (text[start - 1].isalnum() or text[start - 1] in "._"):
            start -= 1
        word = text[start:]
        options: list[str] = []
        state = 0
        while True:
            option = self._completer.complete(word, state)
            if option is None:
                break
            options.append(option)
            state += 1
        if not options:
            return text
        if len(options) == 1:
            return text[:start] + options[0]
        self.write("  ".join(options) + "\n")
        common = options[0]
        for option in options[1:]:
            while not option.startswith(common):
                common = common[:-1]
        return text[:start] + common
