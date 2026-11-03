"""The oscilloscope (phase_10.md 4.4): left above right, a zero-crossing trigger so a tone
stands still, and hold to freeze it."""

from __future__ import annotations

from PySide6.QtWidgets import QCheckBox, QComboBox, QLabel, QWidget

from adx.engine_bridge import AnalyzerFrame
from adx.panels.analyzers.base import AnalyzerPanel
from adx.panels.analyzers.host import AnalyzerHost
from adx.panels.geometry_view import WORLD_LINES

#: The window, in milliseconds at 48 kHz.
WINDOWS_MS: tuple[int, ...] = (5, 10, 20, 50, 100)


class ScopePanel(AnalyzerPanel):
    panel_id = "scope"
    title = "Oscilloscope"
    kind = "scope"
    layers = (("scope", WORLD_LINES),)

    def __init__(self, host: AnalyzerHost, parent: QWidget | None = None) -> None:
        super().__init__(host, parent)
        self.window_box = QComboBox()
        self.window_box.setObjectName("scope.window")
        self.window_box.addItems([f"{ms} ms" for ms in WINDOWS_MS])
        self.window_box.setCurrentText("20 ms")
        self.trigger = QCheckBox("Trigger")
        self.trigger.setObjectName("scope.trigger")
        self.trigger.setChecked(True)
        self.hold = QCheckBox("Hold")
        self.hold.setObjectName("scope.hold")
        self.options.addWidget(QLabel("Window"))
        self.options.addWidget(self.window_box)
        self.options.addWidget(self.trigger)
        self.options.addWidget(self.hold)
        self.options.addStretch(1)
        self.window_box.currentTextChanged.connect(lambda _t: self.apply())
        self.trigger.toggled.connect(lambda _b: self.apply())
        self.hold.toggled.connect(lambda _b: self.apply())

    def settings(self) -> dict[str, object]:
        ms = int(self.window_box.currentText().split()[0])
        return {
            "scope_frames": ms * self.host.engine.sample_rate // 1000,
            "scope_trigger": self.trigger.isChecked(),
            "scope_hold": self.hold.isChecked(),
        }

    def show_readouts(self, frame: AnalyzerFrame) -> None:
        self.readout.setText("held" if self.hold.isChecked() else "")
