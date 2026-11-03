"""The stereo vectorscope and phase correlation (phase_10.md 4.4): mono is a vertical
line, out of phase a horizontal one; the meter under it reads -1 to +1."""

from __future__ import annotations

from PySide6.QtWidgets import QComboBox, QLabel, QWidget

from adx.engine_bridge import AnalyzerFrame
from adx.panels.analyzers.base import AnalyzerPanel
from adx.panels.analyzers.host import AnalyzerHost
from adx.panels.geometry_view import WORLD_LINES, WORLD_TRIANGLES

GAINS: tuple[str, ...] = ("1x", "2x", "4x")


class VectorscopePanel(AnalyzerPanel):
    panel_id = "vectorscope"
    title = "Vectorscope"
    kind = "vector"
    layers = (("vector.trace", WORLD_LINES), ("vector.meter", WORLD_TRIANGLES))
    world = (-1.1, -1.05, 1.1, 1.3)

    def __init__(self, host: AnalyzerHost, parent: QWidget | None = None) -> None:
        super().__init__(host, parent)
        self.gain = QComboBox()
        self.gain.setObjectName("vectorscope.gain")
        self.gain.addItems(list(GAINS))
        self.options.addWidget(QLabel("Gain"))
        self.options.addWidget(self.gain)
        self.options.addStretch(1)
        self.gain.currentTextChanged.connect(lambda _t: self.apply())
        self.correlation = 0.0

    def settings(self) -> dict[str, object]:
        return {"vector_gain": float(self.gain.currentText().rstrip("x"))}

    def show_readouts(self, frame: AnalyzerFrame) -> None:
        self.correlation = frame.readouts[0]
        self.readout.setText(f"correlation {self.correlation:+.2f}")
