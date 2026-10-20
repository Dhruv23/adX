"""Appearance: the theme and the meters' floor."""

from __future__ import annotations

from PySide6.QtWidgets import QComboBox, QFormLayout, QWidget

from adx.preferences import Preferences
from adx.theme.tokens import THEMES

FLOORS = (-48.0, -60.0, -72.0, -96.0)


class AppearancePage(QWidget):
    """Theme and meter scale."""

    title = "Appearance"

    def __init__(self, prefs: Preferences, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.prefs = prefs
        form = QFormLayout(self)
        self.theme = QComboBox()
        self.theme.addItems(list(THEMES))
        self.theme.setCurrentText(prefs.theme)
        form.addRow("Theme", self.theme)
        self.floor = QComboBox()
        self.floor.addItems([f"{f:.0f} dB" for f in FLOORS])
        self.floor.setCurrentIndex(
            FLOORS.index(prefs.meter_floor_db) if prefs.meter_floor_db in FLOORS else 1
        )
        form.addRow("Meter floor", self.floor)

    def apply(self) -> None:
        """Write this page's fields."""
        self.prefs.theme = self.theme.currentText()
        self.prefs.meter_floor_db = FLOORS[self.floor.currentIndex()]
