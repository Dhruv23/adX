"""The routing matrix: the insert-to-insert graph as a grid (phase_6.md 4.3).

Row is the source, column the target. A cell shows a route, a send or a sidechain key -
the last drawn distinctly, because a sidechain that looks like an audio edge is
confusing. A cell that would close a cycle is disabled: the engine's own reachability
over the same edges the graph builder sorts says so *before* any command exists, so an
invalid route cannot be expressed rather than being rejected after the fact
(``mixer_routing_prevents_cycle``). Clicking an enabled cell adds or removes a route.
"""

from __future__ import annotations

import numpy as np
import numpy.typing as npt
from PySide6.QtCore import QRectF, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import QWidget

from adx.theme.tokens import DARK, Theme

NONE, ROUTE, SEND, SIDECHAIN, CYCLE, SELF = range(6)
CELL = 18
HEADER = 28


class RoutingMatrix(QWidget):
    """Emits ``toggled(source, target)`` for a click on an enabled cell."""

    toggled = Signal(int, int)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.ids: list[int] = []
        self.states: npt.NDArray[np.uint8] = np.zeros((0, 0), dtype=np.uint8)
        self._theme: Theme = DARK
        self.issued: list[tuple[int, int]] = []

    def show_matrix(self, ids: list[int], states: npt.NDArray[np.uint8]) -> None:
        """A fresh read of the matrix (Project.routing_matrix)."""
        self.ids, self.states = ids, states
        side = HEADER + CELL * len(ids)
        self.setFixedSize(side, side)
        self.update()

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()

    def cell_at(self, x: float, y: float) -> tuple[int, int] | None:
        """(row, column) under a pixel, or None."""
        row, column = int((y - HEADER) // CELL), int((x - HEADER) // CELL)
        if (
            x < HEADER
            or y < HEADER
            or not (0 <= row < len(self.ids) and 0 <= column < len(self.ids))
        ):
            return None
        return row, column

    def enabled(self, row: int, column: int) -> bool:
        """Whether clicking the cell does anything: not a cycle, not itself, and not an
        edge this grid does not own (a send or a key is edited where it lives)."""
        return int(self.states[row, column]) in (NONE, ROUTE)

    def click(self, row: int, column: int) -> bool:
        """A click on a cell: toggles a route when enabled. Returns whether it did."""
        if not self.enabled(row, column):
            return False
        self.issued.append((self.ids[row], self.ids[column]))
        self.toggled.emit(self.ids[row], self.ids[column])
        return True

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        theme = self._theme
        painter.fillRect(self.rect(), QColor(theme.panel))
        colors = {
            NONE: QColor(theme.raised),
            ROUTE: QColor(theme.accent),
            SEND: QColor(theme.roles["wave_fill"]),
            SIDECHAIN: QColor(theme.roles["curve"]),
            CYCLE: QColor(theme.window),
            SELF: QColor(theme.border),
        }
        painter.setPen(QColor(theme.text_dim))
        for index, insert in enumerate(self.ids):
            painter.drawText(
                QRectF(HEADER + index * CELL, 0, CELL, HEADER),
                Qt.AlignmentFlag.AlignCenter,
                str(insert),
            )
            painter.drawText(
                QRectF(0, HEADER + index * CELL, HEADER - 2, CELL),
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                str(insert),
            )
        for row in range(len(self.ids)):
            for column in range(len(self.ids)):
                state = int(self.states[row, column])
                rect = QRectF(
                    HEADER + column * CELL + 1, HEADER + row * CELL + 1, CELL - 2, CELL - 2
                )
                painter.fillRect(rect, colors.get(state, colors[NONE]))
                if state == SIDECHAIN:
                    # Distinct from an audio edge: a key, drawn as a ring.
                    painter.setPen(QColor(theme.text))
                    painter.drawEllipse(rect.adjusted(4, 4, -4, -4))
                elif state == CYCLE:
                    painter.setPen(QColor(theme.text_dim))
                    painter.drawLine(rect.topLeft(), rect.bottomRight())
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        cell = self.cell_at(event.position().x(), event.position().y())
        if cell is not None:
            self.click(*cell)
