"""MIDI: Phase 9's (plans/phase_9.md). The page is here so the dialog's shape does not
change when it arrives."""

from __future__ import annotations

from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

from adx.preferences import Preferences


class MidiPage(QWidget):
    """A placeholder that says whose it is."""

    title = "MIDI"

    def __init__(self, prefs: Preferences, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.prefs = prefs
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("MIDI input and MIDI learn arrive in Phase 9 (plans/phase_9.md)."))
        layout.addStretch(1)

    def apply(self) -> None:
        """Nothing to write yet."""
