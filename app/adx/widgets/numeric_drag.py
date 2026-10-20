"""A number you drag: vertical drag changes it, shift for fine, double-click resets,
double-click with ctrl types a value. For milliseconds, counts and anything a knob would
make awkward (phase_6.md 4.4: "ms -> numeric drag, '250 ms'")."""

from __future__ import annotations

import math
from collections.abc import Callable

from PySide6.QtCore import QPoint, Qt, Signal
from PySide6.QtGui import QContextMenuEvent, QMouseEvent, QWheelEvent
from PySide6.QtWidgets import QInputDialog, QLabel, QWidget

from adx.widgets.knob import plain


class NumericDrag(QLabel):
    """Emits ``value_changed`` while dragging and ``edit_finished`` once per gesture."""

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
        formatter: Callable[[float], str] = plain,
        integer: bool = False,
        logarithmic: bool = False,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.minimum, self.maximum = minimum, maximum
        self.default = value if default is None else default
        self.label = label
        self.formatter = formatter
        self.integer = integer
        self.logarithmic = logarithmic and minimum > 0 and maximum > minimum
        self._value = value
        self._drag_y: float | None = None
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setCursor(Qt.CursorShape.SizeVerCursor)
        self.setMinimumWidth(64)
        self.set_value(value)

    @property
    def value(self) -> float:
        """The current value."""
        return self._value

    def set_value(self, value: float) -> None:
        """Set without emitting."""
        value = max(self.minimum, min(self.maximum, value))
        self._value = float(round(value)) if self.integer else value
        shown = self.formatter(self._value)
        self.setText(f"{self.label} {shown}".strip())
        self.setToolTip(f"{self.label}: {shown}" if self.label else shown)

    def _fraction(self) -> float:
        if self.maximum <= self.minimum:
            return 0.0
        if self.logarithmic:
            return math.log(self._value / self.minimum) / math.log(self.maximum / self.minimum)
        return (self._value - self.minimum) / (self.maximum - self.minimum)

    def _at(self, fraction: float) -> float:
        f = max(0.0, min(1.0, fraction))
        if self.logarithmic:
            return float(self.minimum * (self.maximum / self.minimum) ** f)
        return self.minimum + f * (self.maximum - self.minimum)

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_y = event.position().y()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._drag_y is None:
            return
        dy = self._drag_y - event.position().y()
        self._drag_y = event.position().y()
        fine = 0.1 if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else 1.0
        self.set_value(self._at(self._fraction() + dy / 200.0 * fine))
        self.value_changed.emit(self._value)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._drag_y is not None:
            self.edit_finished.emit(self._value)
        self._drag_y = None

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
            value, ok = QInputDialog.getDouble(
                self, self.label or "Value", "Value", self._value, self.minimum, self.maximum, 4
            )
            if not ok:
                return
            self.set_value(value)
        else:
            self.set_value(self.default)
        self.value_changed.emit(self._value)
        self.edit_finished.emit(self._value)

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802 - Qt's name
        steps = event.angleDelta().y() / 120
        delta = steps if self.integer else steps / 50.0
        if self.integer:
            self.set_value(self._value + delta)
        else:
            self.set_value(self._at(self._fraction() + delta))
        self.value_changed.emit(self._value)
        self.edit_finished.emit(self._value)

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:  # noqa: N802 - Qt's name
        self.context_requested.emit(event.globalPos())
