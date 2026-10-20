"""A vertical gain fader, in decibels, with a unity mark; double-click resets."""

from __future__ import annotations

import math
from collections.abc import Callable

from PySide6.QtCore import QPoint, QRectF, Qt, Signal
from PySide6.QtGui import (
    QColor,
    QContextMenuEvent,
    QMouseEvent,
    QPainter,
    QPaintEvent,
    QWheelEvent,
)
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
        self.value_changed.emit(self.gain)
        self.edit_finished.emit(self.gain)

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()


class ValueFader(QWidget):
    """A vertical fader over any range - a parameter in dB, say - with its value printed
    under it. The generic editor's control for dB (phase_6.md 4.4)."""

    value_changed = Signal(float)
    edit_finished = Signal(float)
    context_requested = Signal(QPoint)

    def __init__(
        self,
        minimum: float,
        maximum: float,
        value: float,
        default: float | None = None,
        label: str = "",
        formatter: Callable[[float], str] = lambda v: f"{v:.3g}",
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.minimum, self.maximum = minimum, maximum
        self.default = value if default is None else default
        self.label = label
        self.formatter = formatter
        self._value = value
        self._dragging = False
        self._theme: Theme = DARK
        self.setMinimumSize(40, 96)
        self.set_value(value)

    @property
    def value(self) -> float:
        """The current value."""
        return self._value

    def set_value(self, value: float) -> None:
        """Move without emitting."""
        self._value = max(self.minimum, min(self.maximum, value))
        text = self.formatter(self._value)
        self.setToolTip(f"{self.label}: {text}" if self.label else text)
        self.update()

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()

    def _travel(self) -> tuple[float, float]:
        return 6.0, max(1.0, self.height() - 34.0)

    def _from_y(self, y: float) -> None:
        top, travel = self._travel()
        fraction = max(0.0, min(1.0, 1.0 - (y - top) / travel))
        self.set_value(self.minimum + fraction * (self.maximum - self.minimum))
        self.value_changed.emit(self._value)

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        theme = self._theme
        top, travel = self._travel()
        cx = self.width() / 2
        painter.fillRect(QRectF(cx - 2, top, 4, travel), QColor(theme.raised))
        span = self.maximum - self.minimum
        fraction = 0.0 if span <= 0 else (self._value - self.minimum) / span
        y = top + (1.0 - fraction) * travel
        painter.setBrush(QColor(theme.accent))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(QRectF(cx - 9, y - 4, 18, 8), 3, 3)
        painter.setPen(QColor(theme.text_dim))
        painter.drawText(
            QRectF(0, self.height() - 26, self.width(), 13),
            Qt.AlignmentFlag.AlignCenter,
            self.formatter(self._value),
        )
        painter.drawText(
            QRectF(0, self.height() - 13, self.width(), 13),
            Qt.AlignmentFlag.AlignCenter,
            self.label,
        )
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if event.button() == Qt.MouseButton.LeftButton:
            self._dragging = True
            self._from_y(event.position().y())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._dragging:
            self._from_y(event.position().y())

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._dragging:
            self._dragging = False
            self.edit_finished.emit(self._value)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self.set_value(self.default)
        self.value_changed.emit(self._value)
        self.edit_finished.emit(self._value)

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:  # noqa: N802 - Qt's name
        self.context_requested.emit(event.globalPos())
