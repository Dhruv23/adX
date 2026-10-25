"""Playhead-synced step highlighting (phase_7.md 4.6) - the archived LiveCodeEditor's best
idea, and what makes live coding feel live.

While the transport rolls, every mini-notation step that is sounding is lit in the text.
What sounds is the engine's: the compiler's realised notes for each placement of the
pattern (``mini_realize``: the same function the snapshot builder plays), each carrying
the byte span of the step token that made it (its StepSpan). Those are fetched when the
project or the text changes - never per frame. Per frame (60 Hz) this is a numpy
comparison against the playhead, so the frame stays at its one engine call (phase_5.md
4.9).

A block is lit only while the text in the editor is the text that is playing: an edit
not yet applied would otherwise light the wrong characters.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
import numpy.typing as npt
from PySide6.QtGui import QColor, QTextCharFormat, QTextCursor
from PySide6.QtWidgets import QPlainTextEdit, QTextEdit

from adx import engine_bridge
from adx.engine_bridge import Project
from adx.panels.editor.highlighter import to_utf16
from adx.theme.tokens import Theme


def strip_comment(text: str) -> str:
    """The format's comment rule (engine/format/adx/Lexer.cpp): ``#`` starts a comment
    only at a line's start or after whitespace, and never inside quotes."""
    quoted = False
    escaped = False
    for index, char in enumerate(text):
        if escaped:
            escaped = False
            continue
        if char == "\\":
            escaped = True
        elif char == '"':
            quoted = not quoted
        elif char == "#" and not quoted and (index == 0 or text[index - 1] in " \t"):
            return text[:index]
    return text


def _unquote(name: str) -> str:
    name = name.strip()
    if name.startswith('"') and name.endswith('"') and len(name) >= 2:
        return name[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    return name


@dataclass
class MiniBlock:
    """One MINI block as it stands in the editor."""

    pattern: str
    channel: str
    #: (block number, UTF-16 column where its content starts, content)
    lines: list[tuple[int, int, str]] = field(default_factory=list)
    #: Byte offset of each line's content in the joined text.
    starts: list[int] = field(default_factory=list)
    #: Realised notes: rows of mini_realize.
    notes: npt.NDArray[np.int64] = field(default_factory=lambda: np.zeros((0, 8), np.int64))

    def text(self) -> str:
        """The lines joined as the compiler joins them."""
        return " ".join(content for _block, _column, content in self.lines)

    def locate(self, offset: int, length: int) -> tuple[int, int, int] | None:
        """A span of the joined text as (block, UTF-16 start, UTF-16 length)."""
        for index in range(len(self.lines) - 1, -1, -1):
            if self.starts[index] <= offset:
                block, column, content = self.lines[index]
                within = offset - self.starts[index]
                encoded = content.encode("utf-8")
                if within > len(encoded):
                    return None
                start, size = to_utf16(content, within, min(length, len(encoded) - within))
                return block, column + start, size
        return None


def find_blocks(text: str) -> list[MiniBlock]:
    """Every MINI block in ``text``, by the format's rules: under a ``[PATTERN name]``
    header, a ``MINI <channel>`` line, then the lines indented deeper than it."""
    blocks: list[MiniBlock] = []
    pattern = ""
    current: MiniBlock | None = None
    opener_indent = 0
    for number, raw in enumerate(text.splitlines()):
        content = strip_comment(raw).rstrip()
        stripped = content.lstrip(" \t")
        indent = len(content) - len(stripped)
        if not stripped:
            continue
        if raw.startswith("["):
            current = None
            header = raw.strip()
            if header.startswith("[PATTERN") and header.endswith("]"):
                pattern = header[len("[PATTERN") : -1].strip()
            else:
                pattern = ""
            continue
        if current is not None and indent > opener_indent:
            current.starts.append(sum(len(c.encode("utf-8")) + 1 for _b, _c, c in current.lines))
            current.lines.append((number, indent, stripped))
            continue
        current = None
        if pattern and (stripped == "MINI" or stripped.startswith(("MINI ", "MINI\t"))):
            current = MiniBlock(pattern=pattern, channel=_unquote(stripped[4:]))
            opener_indent = indent
            blocks.append(current)
    return [block for block in blocks if block.lines]


class StepMarks:
    """Which steps are sounding, and the selections that light them."""

    def __init__(self) -> None:
        self.blocks: list[MiniBlock] = []

    def rebuild(self, project: Project, text: str) -> None:
        """Re-read what is playing: after an edit to the project or the text."""
        playing = {
            (pattern, channel): engine_bridge.mini_join(lines)
            for pattern, channel, lines in engine_bridge.mini_sources(project)
        }
        cycle = engine_bridge.mini_cycle_length(project) if playing else 0
        self.blocks = []
        for block in find_blocks(text):
            joined = block.text()
            if playing.get((block.pattern, block.channel)) != joined:
                continue  # the editor's text is not what is playing (yet)
            rows = [
                engine_bridge.mini_realize(joined, int(start), int(end), cycle)
                for start, end, _offset in project.pattern_placements(block.pattern).tolist()
            ]
            block.notes = np.concatenate(rows) if rows else np.zeros((0, 8), np.int64)
            self.blocks.append(block)

    def active(self, ticks: int) -> list[tuple[int, int, int]]:
        """Every sounding step as (block number, UTF-16 start, UTF-16 length)."""
        out: list[tuple[int, int, int]] = []
        for block in self.blocks:
            notes = block.notes
            if notes.size == 0:
                continue
            sounding = notes[(notes[:, 0] <= ticks) & (ticks < notes[:, 0] + notes[:, 1])]
            seen: set[tuple[int, int]] = set()
            for span_start, span_length in sounding[:, 4:6].tolist():
                if (span_start, span_length) in seen:
                    continue
                seen.add((span_start, span_length))
                place = block.locate(int(span_start), int(span_length))
                if place is not None:
                    out.append(place)
        return out

    def selections(
        self, editor: QPlainTextEdit, ticks: int, theme: Theme
    ) -> list[QTextEdit.ExtraSelection]:
        """The background marks for ``ticks``."""
        out: list[QTextEdit.ExtraSelection] = []
        document = editor.document()
        colour = QColor(theme.syntax.get("step-active", theme.accent))
        for number, start, length in self.active(ticks):
            block = document.findBlockByNumber(number)
            if not block.isValid():
                continue
            selection = QTextEdit.ExtraSelection()
            fmt = QTextCharFormat()
            fmt.setBackground(colour)
            selection.format = fmt
            cursor = QTextCursor(document)
            cursor.setPosition(block.position() + start)
            cursor.setPosition(block.position() + start + length, QTextCursor.MoveMode.KeepAnchor)
            selection.cursor = cursor
            out.append(selection)
        return out
