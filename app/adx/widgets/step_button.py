"""A small checkable text button: mute, solo, bypass, invert - the toggles on a channel
row or a mixer strip. One click is one edit."""

from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtWidgets import QToolButton, QWidget


class StepButton(QToolButton):
    """Emits ``toggled_by_user(bool)`` only for clicks, never for ``set_checked``."""

    toggled_by_user = Signal(bool)

    def __init__(self, text: str, tooltip: str = "", parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setText(text)
        self.setCheckable(True)
        self.setToolTip(tooltip or text)
        self.setFixedSize(22, 20)
        self.clicked.connect(self.toggled_by_user)

    def set_checked(self, checked: bool) -> None:
        """Show a state without emitting."""
        if self.isChecked() != checked:
            self.blockSignals(True)
            self.setChecked(checked)
            self.blockSignals(False)
