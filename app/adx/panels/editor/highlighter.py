"""Syntax highlighting from the engine's own tokens (phase_7.md 4.6).

The highlighter does not know the grammar. It asks the engine - ``semantic_tokens``,
built on the lexer the parser reads (engine/format/adx/Tokenize.h) - and paints what it
is told. A regex highlighter drifts from the parser within weeks; the archive's
LiveCodeEditor, which found PATTERN= lines by string prefix, is the cautionary tale.

One engine call per edit, not per line: the whole document's tokens are fetched when
the text changes (the document's revision moves) and kept, by line, until it does again.
Columns arrive in UTF-8 bytes and are converted to the document's UTF-16 positions, which
differ only on a line with non-ASCII text.
"""

from __future__ import annotations

from PySide6.QtGui import QColor, QFont, QSyntaxHighlighter, QTextCharFormat, QTextDocument

from adx import engine_bridge
from adx.theme.tokens import DARK, Theme

#: Kinds drawn bold or italic as well as coloured.
_BOLD = {"section-type", "section-name", "keyword"}
_ITALIC = {"comment"}


def utf16_columns(line: str) -> list[int]:
    """For each UTF-8 byte offset into ``line`` (and one past the end), the UTF-16
    position it falls at."""
    columns: list[int] = []
    position = 0
    for char in line:
        width = len(char.encode("utf-8"))
        columns.extend([position] * width)
        position += 2 if ord(char) > 0xFFFF else 1
    columns.append(position)
    return columns


def to_utf16(line: str, column: int, length: int) -> tuple[int, int]:
    """A (byte column, byte length) span on ``line`` as (start, length) in UTF-16."""
    if line.isascii():
        return column, length
    columns = utf16_columns(line)
    start = columns[min(column, len(columns) - 1)]
    end = columns[min(column + length, len(columns) - 1)]
    return start, end - start


class EngineHighlighter(QSyntaxHighlighter):
    """Colours an .adx document by the engine's semantic tokens."""

    def __init__(self, document: QTextDocument, theme: Theme = DARK) -> None:
        super().__init__(document)
        self._kinds = engine_bridge.semantic_kinds()
        self._formats: dict[str, QTextCharFormat] = {}
        self._by_line: dict[int, list[tuple[int, int, int]]] = {}
        self._revision = -1
        self.fetches = 0
        self.set_theme(theme)

    def set_theme(self, theme: Theme) -> None:
        """Rebuild the formats and repaint."""
        self._formats = {}
        for kind in self._kinds:
            fmt = QTextCharFormat()
            fmt.setForeground(QColor(theme.syntax.get(kind, theme.text)))
            if kind in _BOLD:
                fmt.setFontWeight(QFont.Weight.Bold)
            if kind in _ITALIC:
                fmt.setFontItalic(True)
            self._formats[kind] = fmt
        self.rehighlight()

    def _refresh(self) -> None:
        document = self.document()
        if document is None or document.revision() == self._revision:
            return
        self._revision = document.revision()
        self.fetches += 1
        tokens = engine_bridge.semantic_tokens(document.toPlainText())
        by_line: dict[int, list[tuple[int, int, int]]] = {}
        for line, column, length, kind in tokens.tolist():
            by_line.setdefault(int(line), []).append((int(column), int(length), int(kind)))
        self._by_line = by_line

    def tokens_of_block(self, number: int, text: str) -> list[tuple[int, int, str]]:
        """The (start, length, kind) spans block ``number`` is painted with, in UTF-16."""
        self._refresh()
        out: list[tuple[int, int, str]] = []
        for column, length, kind in self._by_line.get(number + 1, []):
            start, size = to_utf16(text, column, length)
            out.append((start, size, self._kinds[kind]))
        return out

    def highlightBlock(self, text: str) -> None:  # noqa: N802 - Qt's name
        """Paint one line."""
        for start, length, kind in self.tokens_of_block(self.currentBlock().blockNumber(), text):
            fmt = self._formats.get(kind)
            if fmt is not None:
                self.setFormat(start, length, fmt)
