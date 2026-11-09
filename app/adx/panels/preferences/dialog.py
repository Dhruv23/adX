"""The preferences dialog (phase_6.md 4.9): five pages over one Preferences value; OK
writes it, Cancel forgets it. A sixth, Shortcuts (phase_11.6.md 4.4), edits the keymap,
which saves each rebind as it is made."""

from __future__ import annotations

import dataclasses
from typing import TYPE_CHECKING

from PySide6.QtWidgets import QDialog, QDialogButtonBox, QTabWidget, QVBoxLayout, QWidget

from adx.panels.preferences.pages.appearance import AppearancePage
from adx.panels.preferences.pages.audio import AudioPage
from adx.panels.preferences.pages.editing import EditingPage
from adx.panels.preferences.pages.midi import MidiPage
from adx.panels.preferences.pages.paths import PathsPage
from adx.panels.preferences.pages.shortcuts import ShortcutsPage
from adx.preferences import Preferences

if TYPE_CHECKING:
    from adx.actions import ActionRegistry

#: Every page has a ``title`` and an ``apply()`` that writes its fields back.
Page = AudioPage | MidiPage | PathsPage | AppearancePage | EditingPage | ShortcutsPage


class PreferencesDialog(QDialog):
    """Edits a copy; :attr:`result_preferences` is it after OK."""

    def __init__(
        self,
        prefs: Preferences,
        latency_ms: float,
        parent: QWidget | None = None,
        midi_inputs: list[str] | None = None,
        midi_outputs: list[str] | None = None,
        registry: ActionRegistry | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("Preferences")
        self.prefs = dataclasses.replace(prefs)
        self.result_preferences: Preferences | None = None
        layout = QVBoxLayout(self)
        self.tabs = QTabWidget()
        self.pages: list[Page] = [
            AudioPage(self.prefs, latency_ms),
            MidiPage(self.prefs, midi_inputs, midi_outputs),
            PathsPage(self.prefs),
            AppearancePage(self.prefs),
            EditingPage(self.prefs),
        ]
        if registry is not None:
            self.shortcuts: ShortcutsPage | None = ShortcutsPage(registry)
            self.pages.append(self.shortcuts)
        else:
            self.shortcuts = None
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
