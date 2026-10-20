"""A colour you click to change: a channel's, a track's, a pattern's. Emits the chosen
colour as ``#rrggbb`` - the format the commands take."""

from __future__ import annotations

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import QColorDialog, QWidget


class ColorSwatch(QWidget):
    """A small filled square."""

    color_chosen = Signal(str)

    def __init__(self, color: str = "#808080", parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._color = color
        self.setFixedSize(14, 14)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setToolTip("Colour")

    @property
    def color(self) -> str:
        """``#rrggbb``."""
        return self._color

    def set_color(self, color: str) -> None:
        """Show a colour without emitting."""
        if color != self._color:
            self._color = color
            self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(self._color))
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        chosen = QColorDialog.getColor(QColor(self._color), self, "Colour")
        if chosen.isValid():
            self.set_color(chosen.name())
            self.color_chosen.emit(chosen.name())
