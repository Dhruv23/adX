"""Make or change a custom scale: twelve semitone toggles and a name (phase_11.6.md 4.1).

The dialog edits a root-relative mask, the shape ScaleHighlight already takes, so the
row colouring and the snapping never know a scale is custom.
"""

from __future__ import annotations

from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from adx.panels.piano_roll import scales


class CustomScaleDialog(QDialog):
    """Save writes through :class:`scales.CustomScales`; Delete removes the name."""

    def __init__(
        self,
        store: scales.CustomScales,
        name: str = "",
        root: int = 0,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("Custom scale")
        self.store = store
        #: What happened: "saved", "deleted" or "" (cancelled).
        self.outcome = ""
        self.saved_name = ""
        mask = store.mask(name) or scales.mask_of("Major")
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("Steps above the root (the root is always in the scale):"))
        grid = QGridLayout()
        self.toggles: list[QPushButton] = []
        for step in range(12):
            label = scales.NOTE_NAMES[(root + step) % 12]
            button = QPushButton(f"{step}\n{label}")
            button.setObjectName(f"custom_scale.step{step}")
            button.setCheckable(True)
            button.setChecked(bool(mask & (1 << step)))
            if step == 0:
                button.setChecked(True)
                button.setEnabled(False)
            grid.addWidget(button, 0, step)
            self.toggles.append(button)
        layout.addLayout(grid)
        row = QHBoxLayout()
        row.addWidget(QLabel("Name"))
        self.name_edit = QLineEdit(name)
        self.name_edit.setObjectName("custom_scale.name")
        self.name_edit.textChanged.connect(self._validate)
        row.addWidget(self.name_edit, 1)
        layout.addLayout(row)
        self.error = QLabel("")
        layout.addWidget(self.error)
        buttons = QDialogButtonBox()
        self.save_button = buttons.addButton("Save", QDialogButtonBox.ButtonRole.AcceptRole)
        self.delete_button = buttons.addButton(
            "Delete", QDialogButtonBox.ButtonRole.DestructiveRole
        )
        buttons.addButton(QDialogButtonBox.StandardButton.Cancel)
        self.delete_button.setEnabled(name in store.names())
        self.save_button.clicked.connect(self.save)
        self.delete_button.clicked.connect(self.delete)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self._original = name
        self._validate()

    def scale_mask(self) -> int:
        """The mask the toggles show."""
        out = 0
        for step, button in enumerate(self.toggles):
            if button.isChecked():
                out |= 1 << step
        return out

    def set_mask(self, mask: int) -> None:
        for step, button in enumerate(self.toggles):
            button.setChecked(step == 0 or bool(mask & (1 << step)))

    def _validate(self) -> None:
        name = self.name_edit.text().strip()
        ok = scales.CustomScales.valid_name(name)
        self.error.setText("" if ok or not name else "That name is taken or not allowed.")
        self.save_button.setEnabled(ok)

    def save(self) -> None:
        name = self.name_edit.text().strip()
        if not scales.CustomScales.valid_name(name):
            return
        if self._original and self._original != name and self._original in self.store.names():
            self.store.delete(self._original)
        self.store.save(name, self.scale_mask())
        self.outcome, self.saved_name = "saved", name
        self.accept()

    def delete(self) -> None:
        if self._original:
            self.store.delete(self._original)
        self.outcome = "deleted"
        self.accept()
