"""A rotary knob over a value range; vertical drag turns it, double-click resets.

What every parameter control does (phase_6.md 4.4): shift-drag for fine adjustment,
double-click to reset to the default, the value in a tooltip as it turns, a context
menu (automate, MIDI learn), and - when the parameter is automated - a ring in the
lane's colour. A logarithmic knob (Hz, a Q) turns evenly in octaves, not in hertz.
"""

from __future__ import annotations

import math
from collections.abc import Callable

from PySide6.QtCore import QPoint, QPointF, QRectF, Qt, Signal
from PySide6.QtGui import (
    QColor,
    QContextMenuEvent,
    QMouseEvent,
    QPainter,
    QPaintEvent,
    QPen,
    QWheelEvent,
)
from PySide6.QtWidgets import QToolTip, QWidget

from adx.theme.tokens import DARK, Theme

#: The knob's arc: 270 degrees, starting at the bottom left.
_START, _SWEEP = 225.0, -270.0


def plain(value: float) -> str:
    """The default formatter: three significant digits."""
    return f"{value:.3g}"


class Knob(QWidget):
    """Emits ``value_changed`` while turning and ``edit_finished`` once per gesture."""

    value_changed = Signal(float)
    edit_finished = Signal(float)
    #: Right-click: the global position, for the owner's context menu.
    context_requested = Signal(QPoint)

    def __init__(
        self,
        minimum: float = 0.0,
        maximum: float = 1.0,
        value: float = 0.5,
        default: float | None = None,
        label: str = "",
        parent: QWidget | None = None,
        logarithmic: bool = False,
        formatter: Callable[[float], str] = plain,
    ) -> None:
        super().__init__(parent)
        self.minimum = minimum
        self.maximum = maximum
        self.default = value if default is None else default
        self.label = label
        self.logarithmic = logarithmic and minimum > 0.0 and maximum > minimum
        self.formatter = formatter
        #: Drawn with a ring in this colour when the parameter is automated.
        self.automation_color: str | None = None
        self._value = value
        self._drag_y: float | None = None
        self._theme: Theme = DARK
        self.setMinimumSize(36, 44)
        self._update_tooltip()

    @property
    def value(self) -> float:
        """The current value."""
        return self._value

    def set_value(self, value: float) -> None:
        """Turn without emitting."""
        self._value = max(self.minimum, min(self.maximum, value))
        self._update_tooltip()
        self.update()

    def set_theme(self, theme: Theme) -> None:
        """Recolour."""
        self._theme = theme
        self.update()

    def fraction(self) -> float:
        """The value's position on the arc, 0..1 (logarithmic for a log knob)."""
        if self.maximum <= self.minimum:
            return 0.0
        if self.logarithmic:
            return math.log(self._value / self.minimum) / math.log(self.maximum / self.minimum)
        return (self._value - self.minimum) / (self.maximum - self.minimum)

    def value_at(self, fraction: float) -> float:
        """The value at an arc position."""
        f = max(0.0, min(1.0, fraction))
        if self.logarithmic:
            return float(self.minimum * (self.maximum / self.minimum) ** f)
        return self.minimum + f * (self.maximum - self.minimum)

    def _update_tooltip(self) -> None:
        text = self.formatter(self._value)
        self.setToolTip(f"{self.label}: {text}" if self.label else text)

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 - Qt's name
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        theme = self._theme
        size = min(self.width(), self.height() - 12) - 6
        rect = QRectF((self.width() - size) / 2, 3, size, size)
        if self.automation_color:
            painter.setPen(QPen(QColor(self.automation_color), 2))
            painter.drawEllipse(rect.adjusted(-2, -2, 2, 2))
        painter.setPen(QPen(QColor(theme.raised), 4))
        painter.drawArc(rect, int(_START * 16), int(_SWEEP * 16))
        painter.setPen(QPen(QColor(theme.accent), 4))
        painter.drawArc(rect, int(_START * 16), int(_SWEEP * self.fraction() * 16))
        angle = math.radians(_START + _SWEEP * self.fraction())
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
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_y = event.position().y()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._drag_y is None:
            return
        dy = self._drag_y - event.position().y()
        self._drag_y = event.position().y()
        fine = 0.1 if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else 1.0
        self.set_value(self.value_at(self.fraction() + dy / 150.0 * fine))
        self.value_changed.emit(self._value)
        QToolTip.showText(event.globalPosition().toPoint(), self.toolTip(), self)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._drag_y is not None:
            self.edit_finished.emit(self._value)
        self._drag_y = None

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        self.set_value(self.default)
        self.value_changed.emit(self._value)
        self.edit_finished.emit(self._value)

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802 - Qt's name
        steps = event.angleDelta().y() / 120
        self.set_value(self.value_at(self.fraction() + steps / 50.0))
        self.value_changed.emit(self._value)
        self.edit_finished.emit(self._value)

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:  # noqa: N802 - Qt's name
        self.context_requested.emit(event.globalPos())
