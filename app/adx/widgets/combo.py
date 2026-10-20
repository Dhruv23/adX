"""A combo box over a stepped parameter: each item is a value (phase_6.md 4.4: "stepped /
enum -> combo"). Choosing an item is one edit."""

from __future__ import annotations

from PySide6.QtCore import QPoint, Qt, Signal
from PySide6.QtWidgets import QComboBox, QWidget


class ParamCombo(QComboBox):
    """Emits ``value_changed`` and ``edit_finished`` with the chosen item's value."""

    value_changed = Signal(float)
    edit_finished = Signal(float)
    context_requested = Signal(QPoint)

    def __init__(
        self, options: list[tuple[str, float]], value: float, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self._values = [v for _label, v in options]
        for label, _v in options:
            self.addItem(label)
        self.set_value(value)
        self.activated.connect(self._chosen)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.customContextMenuRequested.connect(
            lambda point: self.context_requested.emit(self.mapToGlobal(point))
        )

    @property
    def value(self) -> float:
        """The chosen item's value."""
        index = self.currentIndex()
        return self._values[index] if 0 <= index < len(self._values) else 0.0

    def set_value(self, value: float) -> None:
        """Select the item nearest ``value`` without emitting."""
        if not self._values:
            return
        nearest = min(range(len(self._values)), key=lambda i: abs(self._values[i] - value))
        self.blockSignals(True)
        self.setCurrentIndex(nearest)
        self.blockSignals(False)

    def _chosen(self, index: int) -> None:
        value = self._values[index]
        self.value_changed.emit(value)
        self.edit_finished.emit(value)


def stepped_options(
    minimum: float, maximum: float, names: list[str] | None = None
) -> list[tuple[str, float]]:
    """Items for every integer from minimum to maximum, named when ``names`` says how."""
    out: list[tuple[str, float]] = []
    for index, value in enumerate(range(round(minimum), round(maximum) + 1)):
        label = names[index] if names is not None and index < len(names) else str(value)
        out.append((label, float(value)))
    return out
