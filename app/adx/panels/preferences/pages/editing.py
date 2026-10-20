"""Editing: default note length, snap, undo limit, autosave interval."""

from __future__ import annotations

from PySide6.QtWidgets import QComboBox, QFormLayout, QSpinBox, QWidget

from adx.engine_bridge import PPQ
from adx.preferences import Preferences

LENGTHS = (
    ("1/32", PPQ // 8),
    ("1/16", PPQ // 4),
    ("1/8", PPQ // 2),
    ("1/4", PPQ),
    ("1/2", PPQ * 2),
)


class EditingPage(QWidget):
    """Defaults for editing."""

    title = "Editing"

    def __init__(self, prefs: Preferences, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.prefs = prefs
        form = QFormLayout(self)
        ticks = [t for _l, t in LENGTHS]
        self.note = QComboBox()
        self.note.addItems([label for label, _t in LENGTHS])
        self.note.setCurrentIndex(
            ticks.index(prefs.default_note_ticks) if prefs.default_note_ticks in ticks else 1
        )
        form.addRow("Default note length", self.note)
        self.snap = QComboBox()
        self.snap.addItems([label for label, _t in LENGTHS])
        self.snap.setCurrentIndex(ticks.index(prefs.snap_ticks) if prefs.snap_ticks in ticks else 1)
        form.addRow("Default snap", self.snap)
        self.undo = QSpinBox()
        self.undo.setRange(10, 100000)
        self.undo.setValue(prefs.undo_limit)
        form.addRow("Undo limit", self.undo)
        self.autosave = QSpinBox()
        self.autosave.setRange(0, 120)
        self.autosave.setSuffix(" min")
        self.autosave.setSpecialValueText("off")
        self.autosave.setValue(prefs.autosave_minutes)
        form.addRow("Autosave every", self.autosave)

    def apply(self) -> None:
        """Write this page's fields."""
        ticks = [t for _l, t in LENGTHS]
        self.prefs.default_note_ticks = ticks[self.note.currentIndex()]
        self.prefs.snap_ticks = ticks[self.snap.currentIndex()]
        self.prefs.undo_limit = self.undo.value()
        self.prefs.autosave_minutes = self.autosave.value()
