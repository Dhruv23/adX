"""A rotary knob over a value range; vertical drag turns it, double-click resets."""

from __future__ import annotations

import math

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent, QPen, QWheelEvent
from PySide6.QtWidgets import QWidget

from adx.theme.tokens import DARK, Theme

#: The knob's arc: 270 degrees, starting at the bottom left.
_START, _SWEEP = 225.0, -270.0


class Knob(QWidget):
    """Emits ``value_changed`` while turning and ``edit_finished`` once per gesture."""

    value_changed = Signal(float)
    edit_finished = Signal(float)

    def __init__(
        self,
        minimum: float = 0.0,
        maximum: float = 1.0,
        value: float = 0.5,
        default: float | None = None,
        label: str = "",
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.minimum = minimum
        self.maximum = maximum
        self.default = value if default is None else default
        self.label = label
        self._value = value
        self._drag_y: float | None = None
        self._theme: Theme = DARK
        self.setMinimumSize(36, 44)
        self.setToolTip(label)

    @property
    def value(self) -> float:
        """The current value."""
        return self._value

    def set_value(self, value: float) -> None:
        """Turn without emitting."""
        self._value = max(self.minimum, min(self.maximum, value))
        self.update()

    def _fraction(self) -> float:
        span = self.maximum - self.minimum
        return 0.0 if span <= 0 else (self._value - self.minimum) / span

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        theme = self._theme
        size = min(self.width(), self.height() - 12) - 6
        rect = QRectF((self.width() - size) / 2, 3, size, size)
        painter.setPen(QPen(QColor(theme.raised), 4))
        painter.drawArc(rect, int(_START * 16), int(_SWEEP * 16))
        painter.setPen(QPen(QColor(theme.accent), 4))
        painter.drawArc(rect, int(_START * 16), int(_SWEEP * self._fraction() * 16))
        angle = math.radians(_START + _SWEEP * self._fraction())
        center = rect.center()
        tip = QPointF(
            center.x() + math.cos(angle) * size * 0.38, center.y() - math.sin(angle) * size * 0.38
        )
        painter.setPen(QPen(QColor(theme.text), 2))
        painter.drawLine(center, tip)
        painter.setPen(QColor(theme.text_dim))
        painter.drawText(
            QRectF(0, self.height() - 12, self.width(), 12),
            Qt.AlignmentFlag.AlignCenter,
            self.label,
        )
        painter.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self._drag_y = event.position().y()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._drag_y is None:
            return
        dy = self._drag_y - event.position().y()
        self._drag_y = event.position().y()
        fine = 0.2 if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else 1.0
        self.set_value(self._value + dy / 150.0 * (self.maximum - self.minimum) * fine)
        self.value_changed.emit(self._value)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._drag_y is not None:
            self.edit_finished.emit(self._value)
        self._drag_y = None

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self.set_value(self.default)
        self.edit_finished.emit(self._value)

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802 - Qt's name
        self.set_value(
            self._value + event.angleDelta().y() / 120 * (self.maximum - self.minimum) / 50
        )
        self.edit_finished.emit(self._value)
