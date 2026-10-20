"""Convolution: the impulse response it convolves with (phase_6.md 4.4's table: "needs IR
display").

A slot with an ``ir=`` file shows that file's waveform from the engine's peak cache; one
without shows the synthetic room its size and damping describe. Choosing a file sets it
as the slot's impulse response (one undo step). Trimming the response is not something the
effect can do yet - its file plays whole - and is left for Phase 8's clip editing.
"""

from __future__ import annotations

from PySide6.QtWidgets import QFileDialog, QLabel, QPushButton, QVBoxLayout, QWidget

from adx.panels.editor_parts import WaveView
from adx.panels.effect.generic_editor import slot_info
from adx.panels.instrument.editors.sampler import shared_cache
from adx.session import Session


class ImpulseView(QWidget):
    """The IR's waveform, or a note that the room is synthetic."""

    def __init__(self, session: Session, insert: int, slot: int) -> None:
        super().__init__()
        self.session, self.insert, self.slot = session, insert, slot
        layout = QVBoxLayout(self)
        self.label = QLabel()
        layout.addWidget(self.label)
        self.wave = WaveView(shared_cache())
        layout.addWidget(self.wave, 1)
        choose = QPushButton("Impulse response…")
        choose.clicked.connect(self._choose)
        layout.addWidget(choose)
        self._shown = ""
        self.refresh()

    def refresh(self) -> None:
        """Show the slot's current IR."""
        info = slot_info(self.session, self.insert, self.slot)
        ir = "" if info is None else str(info["ir"])
        self.label.setText(f"IR: {ir}" if ir else "No IR file: a synthetic room (size, damping)")
        if ir != self._shown:
            self._shown = ir
            samples = {path: full for _i, path, full in self.session.require().samples()}
            self.wave.open(samples.get(ir, ""))

    def _choose(self) -> None:
        path, _filter = QFileDialog.getOpenFileName(
            self, "Impulse response", "", "Audio (*.wav *.flac *.aif *.aiff)"
        )
        if path:
            self.session.call(self.session.require().set_slot_impulse, self.slot, path)
