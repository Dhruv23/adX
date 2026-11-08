"""MIDI: which inputs and outputs adX opens (phase_9.md 4.1, STATE.md P9-5). A keyboard
plays the selected channel; its notes reach the instrument at their own sample, never
quantised to a block. Outputs can be sent MIDI clock and time code, and the arrangement
can follow another device's clock or time code instead of its own transport."""

from __future__ import annotations

from PySide6.QtWidgets import QCheckBox, QComboBox, QFormLayout, QLabel, QVBoxLayout, QWidget

from adx.preferences import Preferences

#: What the arrangement follows, as stored and as shown.
FOLLOW = (("none", "Its own transport"), ("clock", "MIDI clock"), ("mtc", "MIDI time code"))


class MidiPage(QWidget):
    """One checkbox per port the system has, plus any remembered one that is unplugged."""

    title = "MIDI"

    def __init__(
        self,
        prefs: Preferences,
        inputs: list[str] | None = None,
        outputs: list[str] | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.prefs = prefs
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("<b>Inputs</b>"))
        self.boxes = self._ports(layout, inputs or [], prefs.midi_inputs, "inputs")
        layout.addWidget(QLabel("An open input plays the selected channel."))

        layout.addWidget(QLabel("<b>Outputs</b>"))
        self.out_boxes = self._ports(layout, outputs or [], prefs.midi_outputs, "outputs")
        self.send_clock = QCheckBox("Send MIDI clock to open outputs")
        self.send_clock.setChecked(prefs.midi_send_clock)
        self.send_mtc = QCheckBox("Send MIDI time code (30 fps) to open outputs")
        self.send_mtc.setChecked(prefs.midi_send_mtc)
        layout.addWidget(self.send_clock)
        layout.addWidget(self.send_mtc)

        form = QFormLayout()
        self.follow = QComboBox()
        for value, label in FOLLOW:
            self.follow.addItem(label, value)
        index = self.follow.findData(prefs.sync_follow)
        self.follow.setCurrentIndex(max(0, index))
        form.addRow("The arrangement follows", self.follow)
        layout.addLayout(form)
        layout.addWidget(
            QLabel("Following uses any open input. Play, stop and position come from it.")
        )
        layout.addStretch(1)

    @staticmethod
    def _ports(
        layout: QVBoxLayout, present: list[str], remembered: list[str], what: str
    ) -> dict[str, QCheckBox]:
        names = list(dict.fromkeys([*present, *remembered]))
        boxes: dict[str, QCheckBox] = {}
        if not names:
            layout.addWidget(QLabel(f"No MIDI {what} found. Plug one in and reopen this page."))
        for name in names:
            box = QCheckBox(name if name in present else f"{name} (not connected)")
            box.setChecked(name in remembered)
            boxes[name] = box
            layout.addWidget(box)
        return boxes

    def apply(self) -> None:
        """Write the checked ports and the sync settings."""
        self.prefs.midi_inputs = [name for name, box in self.boxes.items() if box.isChecked()]
        self.prefs.midi_outputs = [name for name, box in self.out_boxes.items() if box.isChecked()]
        self.prefs.midi_send_clock = self.send_clock.isChecked()
        self.prefs.midi_send_mtc = self.send_mtc.isChecked()
        self.prefs.sync_follow = str(self.follow.currentData())
