"""Two parameters on one surface: drag the dot. A filter's cutoff and resonance, a
panner's pan and width."""

from __future__ import annotations

from PySide6.QtCore import QPointF, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import QWidget

from adx.theme.tokens import DARK, Theme


class XYPad(QWidget):
    """Emits ``moved(x, y)`` while dragging and ``edit_finished(x, y)`` once, both 0..1."""

    moved = Signal(float, float)
    edit_finished = Signal(float, float)

    def __init__(self, x: float = 0.5, y: float = 0.5, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._x, self._y = x, y
        self._dragging = False
        self._theme: Theme = DARK
        self.setMinimumSize(90, 90)

    @property
    def position(self) -> tuple[float, float]:
        """(x, y), each 0..1, y up."""
        return self._x, self._y

    def set_position(self, x: float, y: float) -> None:
        """Move the dot without emitting."""
        self._x, self._y = max(0.0, min(1.0, x)), max(0.0, min(1.0, y))
        self.update()

    def _from(self, point: QPointF) -> None:
        self.set_position(point.x() / max(1, self.width()), 1.0 - point.y() / max(1, self.height()))
        self.moved.emit(self._x, self._y)

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor(self._theme.raised))
        painter.setPen(QColor(self._theme.border))
        painter.drawLine(self.width() // 2, 0, self.width() // 2, self.height())
        painter.drawLine(0, self.height() // 2, self.width(), self.height() // 2)
        painter.setBrush(QColor(self._theme.accent))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawEllipse(QPointF(self._x * self.width(), (1 - self._y) * self.height()), 5, 5)
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self._dragging = True
        self._from(event.position())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._dragging:
            self._from(event.position())

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._dragging:
            self._dragging = False
            self.edit_finished.emit(self._x, self._y)
