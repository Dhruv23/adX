"""Pad setup (phase_11.md 4.7): which open MIDI input is a pad controller, speaking which
profile, and learn mode for a controller no profile ships for.

Learn mode reads what the device sends (Engine.midi_last_control, polled at 20 Hz) and
asks for each pad in turn: the grid row by row from the top left, then the scene
buttons, stop all and tap tempo, any of which can be skipped. The result is saved as a
profile in the settings directory and attached at once.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (
    QComboBox,
    QDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from adx import pads
from adx.pads import Learn, PadControllers


class PadSetupDialog(QDialog):
    """Inputs, profiles, learn."""

    def __init__(
        self, controllers: PadControllers, settings_dir: Path | None, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("Pad controllers")
        self.setObjectName("pad-setup")
        self.controllers = controllers
        self.settings_dir = settings_dir
        self.learn: Learn | None = None
        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.input = QComboBox()
        self.input.setObjectName("pad-input")
        self.input.addItems(controllers.engine.midi_opened)
        self.input.currentTextChanged.connect(self._input_chosen)
        self.profile = QComboBox()
        self.profile.setObjectName("pad-profile")
        self.profiles = pads.profiles(settings_dir)
        self.profile.addItems([p.name for p in self.profiles])
        form.addRow("Input", self.input)
        form.addRow("Profile", self.profile)
        layout.addLayout(form)
        buttons = QHBoxLayout()
        self.attach = QPushButton("Use as pads")
        self.attach.setObjectName("pad-attach")
        self.attach.clicked.connect(self._attach)
        self.detach = QPushButton("Stop using")
        self.detach.setObjectName("pad-detach")
        self.detach.clicked.connect(self._detach)
        for button in (self.attach, self.detach):
            buttons.addWidget(button)
        layout.addLayout(buttons)
        learn = QHBoxLayout()
        self.columns = QSpinBox()
        self.columns.setRange(1, 64)
        self.columns.setValue(8)
        self.rows = QSpinBox()
        self.rows.setRange(1, 64)
        self.rows.setValue(8)
        self.learn_button = QPushButton("Learn…")
        self.learn_button.setObjectName("pad-learn")
        self.learn_button.clicked.connect(self.start_learn)
        self.skip = QPushButton("Skip")
        self.skip.setObjectName("pad-learn-skip")
        self.skip.setEnabled(False)
        self.skip.clicked.connect(self._skip)
        for widget in (QLabel("Grid"), self.columns, QLabel("x"), self.rows, self.learn_button):
            learn.addWidget(widget)
        learn.addWidget(self.skip)
        layout.addLayout(learn)
        self.prompt = QLabel("")
        self.prompt.setObjectName("pad-learn-prompt")
        layout.addWidget(self.prompt)
        self.status = QLabel("")
        self.status.setObjectName("pad-status")
        layout.addWidget(self.status)
        self.timer = QTimer(self)
        self.timer.setInterval(50)
        self.timer.timeout.connect(self.poll)
        self._input_chosen(self.input.currentText())

    def _input_chosen(self, name: str) -> None:
        matched = pads.match(name, self.settings_dir) if name else None
        attached = self.controllers.attached.get(name)
        chosen = attached or matched
        if chosen is not None:
            index = self.profile.findText(chosen.name)
            if index >= 0:
                self.profile.setCurrentIndex(index)
        self.status.setText(f"{name}: pads ({attached.name})" if attached else "")

    def _chosen(self) -> pads.Profile | None:
        index = self.profile.currentIndex()
        return self.profiles[index] if 0 <= index < len(self.profiles) else None

    def _attach(self) -> None:
        name = self.input.currentText()
        profile = self._chosen()
        if not name or profile is None:
            return
        self.controllers.attach(name, profile)
        self.status.setText(f"{name}: pads ({profile.name})")

    def _detach(self) -> None:
        name = self.input.currentText()
        if name:
            self.controllers.detach(name)
            self.status.setText(f"{name}: an ordinary input")

    # --- learn ----------------------------------------------------------------------

    def start_learn(self) -> None:
        name = self.input.currentText()
        base = name.rpartition(" ")[0] if name.rpartition(" ")[2].isdigit() else name
        self.learn = Learn(
            f"{base or 'Pads'} (learned)", self.columns.value(), self.rows.value(), base
        )
        # While learning, a press is a step, not a launch, and the lights stay off.
        if name:
            self.controllers.detach(name)
        # Anything waiting from before the learn began is not a press of it.
        self.controllers.engine.midi_last_control()
        self.skip.setEnabled(True)
        self.prompt.setText(f"Press {self.learn.prompt}")
        self.timer.start()

    def _skip(self) -> None:
        if self.learn is not None:
            self.learn.skip()
            self._advance()

    def poll(self) -> None:
        """One read of what the device sent, as the next step."""
        if self.learn is None:
            return
        control = self.controllers.engine.midi_last_control()
        if control is not None and self.learn.press(control):
            self._advance()

    def _advance(self) -> None:
        if self.learn is None:
            return
        if not self.learn.done:
            self.prompt.setText(f"Press {self.learn.prompt}")
            return
        self.timer.stop()
        self.skip.setEnabled(False)
        text = self.learn.result()
        self.learn = None
        if self.settings_dir is not None:
            path = pads.save_learned(text, self.settings_dir)
            self.profiles = pads.profiles(self.settings_dir)
            self.profile.clear()
            self.profile.addItems([p.name for p in self.profiles])
            learned = pads.Profile.read(path, shipped=False)
            if learned is not None:
                self.profile.setCurrentText(learned.name)
                self._attach()
            self.prompt.setText(f"Learned: saved as {path.name}")
        else:
            self.prompt.setText("Learned")
