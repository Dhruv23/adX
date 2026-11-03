"""MIDI: which inputs adX opens (phase_9.md 4.1). A keyboard plays the selected channel;
its notes reach the instrument at their own sample, never quantised to a block."""

from __future__ import annotations

from PySide6.QtWidgets import QCheckBox, QLabel, QVBoxLayout, QWidget

from adx.preferences import Preferences


class MidiPage(QWidget):
    """One checkbox per input the system has, plus any remembered one that is unplugged."""

    title = "MIDI"

    def __init__(
        self, prefs: Preferences, inputs: list[str] | None = None, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.prefs = prefs
        layout = QVBoxLayout(self)
        names = list(dict.fromkeys([*(inputs or []), *prefs.midi_inputs]))
        self.boxes: dict[str, QCheckBox] = {}
        if not names:
            layout.addWidget(QLabel("No MIDI inputs found. Plug one in and reopen this page."))
        for name in names:
            box = QCheckBox(name if name in (inputs or []) else f"{name} (not connected)")
            box.setChecked(name in prefs.midi_inputs)
            self.boxes[name] = box
            layout.addWidget(box)
        layout.addWidget(QLabel("An open input plays the selected channel."))
        layout.addStretch(1)

    def apply(self) -> None:
        """Write the checked inputs."""
        self.prefs.midi_inputs = [name for name, box in self.boxes.items() if box.isChecked()]
