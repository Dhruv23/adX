"""A vertical gain fader, in decibels, with a unity mark; double-click resets."""

from __future__ import annotations

import math

from PySide6.QtCore import QRectF, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent, QWheelEvent
from PySide6.QtWidgets import QWidget

from adx.theme.tokens import DARK, Theme

MIN_DB, MAX_DB = -60.0, 6.0


def gain_to_position(gain: float) -> float:
    """Linear gain to 0..1 fader travel (dB-linear)."""
    db = MIN_DB if gain <= 1e-6 else 20.0 * math.log10(gain)
    return max(0.0, min(1.0, (db - MIN_DB) / (MAX_DB - MIN_DB)))


def position_to_gain(position: float) -> float:
    """0..1 fader travel back to linear gain; the bottom is silence."""
    if position <= 0.0:
        return 0.0
    return float(10.0 ** ((MIN_DB + position * (MAX_DB - MIN_DB)) / 20.0))


class Fader(QWidget):
    """Emits ``value_changed(gain)`` while dragged, ``edit_finished(gain)`` on release -
    the one command a fader gesture becomes."""

    value_changed = Signal(float)
    edit_finished = Signal(float)

    def __init__(self, gain: float = 1.0, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._position = gain_to_position(gain)
        self._theme: Theme = DARK
        self.setMinimumSize(24, 100)

    @property
    def gain(self) -> float:
        """The current linear gain."""
        return position_to_gain(self._position)

    def set_gain(self, gain: float) -> None:
        """Move without emitting."""
        self._position = gain_to_position(gain)
        self.update()

    def _set_from_y(self, y: float) -> None:
        travel = max(1.0, self.height() - 12)
        self._position = max(0.0, min(1.0, 1.0 - (y - 6) / travel))
        self.update()
        self.value_changed.emit(self.gain)

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        theme = self._theme
        travel = self.height() - 12
        cx = self.width() / 2
        painter.fillRect(QRectF(cx - 2, 6, 4, travel), QColor(theme.raised))
        unity = 6 + (1.0 - gain_to_position(1.0)) * travel
        painter.fillRect(QRectF(cx - 8, unity, 16, 1), QColor(theme.text_dim))
        y = 6 + (1.0 - self._position) * travel
        painter.setBrush(QColor(theme.accent))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(QRectF(cx - 9, y - 5, 18, 10), 3, 3)
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self._set_from_y(event.position().y())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self._set_from_y(event.position().y())

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self.edit_finished.emit(self.gain)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self.set_gain(1.0)
        self.edit_finished.emit(1.0)

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802 - Qt's name
        self._position = max(0.0, min(1.0, self._position + event.angleDelta().y() / 120 * 0.02))
        self.update()
        self.edit_finished.emit(self.gain)
