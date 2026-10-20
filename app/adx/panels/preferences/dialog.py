"""The preferences dialog (phase_6.md 4.9): five pages over one Preferences value; OK
writes it, Cancel forgets it."""

from __future__ import annotations

import dataclasses

from PySide6.QtWidgets import QDialog, QDialogButtonBox, QTabWidget, QVBoxLayout, QWidget

from adx.panels.preferences.pages.appearance import AppearancePage
from adx.panels.preferences.pages.audio import AudioPage
from adx.panels.preferences.pages.editing import EditingPage
from adx.panels.preferences.pages.midi import MidiPage
from adx.panels.preferences.pages.paths import PathsPage
from adx.preferences import Preferences

#: Every page has a ``title`` and an ``apply()`` that writes its fields back.
Page = AudioPage | MidiPage | PathsPage | AppearancePage | EditingPage


class PreferencesDialog(QDialog):
    """Edits a copy; :attr:`result_preferences` is it after OK."""

    def __init__(
        self, prefs: Preferences, latency_ms: float, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("Preferences")
        self.prefs = dataclasses.replace(prefs)
        self.result_preferences: Preferences | None = None
        layout = QVBoxLayout(self)
        self.tabs = QTabWidget()
        self.pages: list[Page] = [
            AudioPage(self.prefs, latency_ms),
            MidiPage(self.prefs),
            PathsPage(self.prefs),
            AppearancePage(self.prefs),
            EditingPage(self.prefs),
        ]
        for page in self.pages:
            self.tabs.addTab(page, page.title)
        layout.addWidget(self.tabs)
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def accept(self) -> None:
        """Every page writes its fields into the copy."""
        for page in self.pages:
            page.apply()
        self.result_preferences = self.prefs
        super().accept()
