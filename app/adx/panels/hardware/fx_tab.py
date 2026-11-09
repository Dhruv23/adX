"""The FM-1's effects by name (phase_11.5.md 4.6, 4.7). adX owns this state: the device
reports none, so a knob turned on the device is not seen here. "Match in adX" puts an
Fm1Effects with the same settings on a channel's insert.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QFormLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    QSlider,
    QVBoxLayout,
    QWidget,
)

from adx.hardware.fm1.manager import Fm1Manager

NOTE = (
    "adX keeps the FM-1's effect settings and sends them on connect. The FM-1 never "
    "reports its own, so changes made with its knobs are not seen here."
)


class FxTab(QWidget):
    def __init__(self, manager: Fm1Manager, match: Callable[[dict[str, int]], bool] | None) -> None:
        super().__init__()
        self.manager = manager
        self.match_hook = match
        layout = QVBoxLayout(self)
        note = QLabel(NOTE)
        note.setWordWrap(True)
        layout.addWidget(note)
        self.match_button = QPushButton("Match in adX")
        self.match_button.setObjectName("fm1.fx.match")
        self.match_button.setToolTip("An FM-1 effects chain with these settings on a channel")
        self.match_button.clicked.connect(self.match)
        layout.addWidget(self.match_button)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        self.body = QWidget()
        self.form = QFormLayout(self.body)
        scroll.setWidget(self.body)
        layout.addWidget(scroll, 1)
        self.sliders: dict[str, QSlider] = {}
        manager.changed.connect(self.refresh)
        self.refresh()

    def refresh(self) -> None:
        device = self.manager.device
        names = device.profile.fx_names() if device is not None else []
        if list(self.sliders) == names:
            for name, slider in self.sliders.items():
                slider.blockSignals(True)
                slider.setValue(device.fx[name] if device is not None else 0)
                slider.blockSignals(False)
            return
        while self.form.rowCount():
            self.form.removeRow(0)
        self.sliders = {}
        for name in names:
            slider = QSlider(Qt.Orientation.Horizontal)
            slider.setObjectName(f"fm1.fx.{name}")
            slider.setRange(0, 127)
            slider.setValue(device.fx[name] if device is not None else 0)
            slider.valueChanged.connect(lambda v, n=name: self.set(n, v))
            self.form.addRow(name.replace("_", " ").title(), slider)
            self.sliders[name] = slider

    def set(self, name: str, value: int) -> None:
        if self.manager.device is not None:
            self.manager.device.set_fx(name, value)

    def match(self) -> bool:
        if self.manager.device is None or self.match_hook is None:
            return False
        return self.match_hook(dict(self.manager.device.fx))
