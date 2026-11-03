"""The spectrum analyzer (phase_10.md 4.4): log frequency, selectable FFT size and window,
peak hold. The FFT runs on the hub's worker, so a 32k transform costs the UI nothing."""

from __future__ import annotations

from PySide6.QtWidgets import QCheckBox, QComboBox, QLabel, QPushButton, QWidget

from adx.engine_bridge import AnalyzerFrame
from adx.panels.analyzers.base import AnalyzerPanel
from adx.panels.analyzers.host import AnalyzerHost
from adx.panels.geometry_view import WORLD_LINES, WORLD_TRIANGLES

FFT_SIZES: tuple[int, ...] = (1024, 2048, 4096, 8192, 16384, 32768)
WINDOWS: tuple[str, ...] = ("hann", "blackman-harris", "hamming", "rectangular")


class SpectrumPanel(AnalyzerPanel):
    panel_id = "spectrum"
    title = "Spectrum"
    kind = "spectrum"
    layers = (("spectrum.fill", WORLD_TRIANGLES), ("spectrum.lines", WORLD_LINES))

    def __init__(self, host: AnalyzerHost, parent: QWidget | None = None) -> None:
        super().__init__(host, parent)
        self.fft_size = QComboBox()
        self.fft_size.setObjectName("spectrum.fft_size")
        self.fft_size.addItems([str(n) for n in FFT_SIZES])
        self.fft_size.setCurrentText("8192")
        self.window_box = QComboBox()
        self.window_box.setObjectName("spectrum.window")
        self.window_box.addItems(list(WINDOWS))
        self.peak = QCheckBox("Peak hold")
        self.peak.setObjectName("spectrum.peak_hold")
        self.peak.setChecked(True)
        self.reset = QPushButton("Reset peaks")
        self.reset.setObjectName("spectrum.reset")
        self.reset.clicked.connect(self._reset_peaks)
        for label, widget in (("FFT", self.fft_size), ("Window", self.window_box)):
            self.options.addWidget(QLabel(label))
            self.options.addWidget(widget)
        self.options.addWidget(self.peak)
        self.options.addWidget(self.reset)
        self.options.addStretch(1)
        self.fft_size.currentTextChanged.connect(lambda _t: self.apply())
        self.window_box.currentTextChanged.connect(lambda _t: self.apply())
        self.peak.toggled.connect(lambda _b: self.apply())

    def settings(self) -> dict[str, object]:
        return {
            "fft_size": int(self.fft_size.currentText()),
            "window": self.window_box.currentText(),
            "peak_hold": self.peak.isChecked(),
        }

    def _reset_peaks(self) -> None:
        if self.kind in self.host.open:
            self.host.configure(reset_peaks=True)

    def show_readouts(self, frame: AnalyzerFrame) -> None:
        self.readout.setText(
            f"{self.fft_size.currentText()}-point {self.window_box.currentText()}, 20 Hz - 20 kHz"
        )
