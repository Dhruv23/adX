"""Make me a track in this style (phase_10.md 4.6): pick a suite, a seed, a length, and
optionally a tempo and key; the track, or just the style's template, opens as a new
project. Riffs inside a pattern stay where Phase 5 put them, in the piano roll."""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from adx import generate
from adx.panels.base import DockPanel

STYLE_KEY = "The style's own"
KEYS = [
    f"{name} {mode}"
    for mode in ("major", "minor")
    for name in ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
]


class GeneratePanel(DockPanel):
    panel_id = "generate"
    title = "Generate"
    scrolls = True
    default_area = Qt.DockWidgetArea.LeftDockWidgetArea

    def __init__(
        self, open_text: Callable[[str, str], bool], parent: QWidget | None = None
    ) -> None:
        """``open_text(text, title)`` opens .adx text as a new project (the main window's)."""
        super().__init__(parent)
        self.open_text = open_text
        self.suites = generate.styles()
        body = QWidget()
        layout = QVBoxLayout(body)
        form = QFormLayout()
        self.style_box = QComboBox()
        self.style_box.setObjectName("generate.style")
        for suite in self.suites:
            self.style_box.addItem(suite.name, suite.id)
        self.description = QLabel("")
        self.description.setObjectName("generate.description")
        self.description.setWordWrap(True)
        self.seed = QSpinBox()
        self.seed.setObjectName("generate.seed")
        self.seed.setRange(1, 1_000_000)
        self.bars = QSpinBox()
        self.bars.setObjectName("generate.bars")
        self.bars.setRange(8, 256)
        self.bars.setValue(32)
        self.bpm = QDoubleSpinBox()
        self.bpm.setObjectName("generate.bpm")
        self.bpm.setRange(0.0, 300.0)
        self.bpm.setSpecialValueText("the style's own")
        self.key = QComboBox()
        self.key.setObjectName("generate.key")
        self.key.addItems([STYLE_KEY, *KEYS])
        form.addRow("Style", self.style_box)
        form.addRow("", self.description)
        form.addRow("Seed", self.seed)
        form.addRow("Bars", self.bars)
        form.addRow("Tempo", self.bpm)
        form.addRow("Key", self.key)
        layout.addLayout(form)
        buttons = QHBoxLayout()
        self.make = QPushButton("Make a track")
        self.make.setObjectName("generate.make")
        self.make.clicked.connect(lambda: self.run(notes=True))
        self.bare = QPushButton("Template only")
        self.bare.setObjectName("generate.template")
        self.bare.clicked.connect(lambda: self.run(notes=False))
        buttons.addWidget(self.make)
        buttons.addWidget(self.bare)
        layout.addLayout(buttons)
        self.status = QLabel("")
        self.status.setObjectName("generate.status")
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        layout.addStretch(1)
        self.setWidget(body)
        self.style_box.currentIndexChanged.connect(self._describe)
        self._describe()

    def _describe(self) -> None:
        index = self.style_box.currentIndex()
        self.description.setText(self.suites[index].description if index >= 0 else "")

    def run(self, *, notes: bool) -> generate.Generated | None:
        style = str(self.style_box.currentData())
        bpm = self.bpm.value() or None
        key = None if self.key.currentText() == STYLE_KEY else self.key.currentText()
        make = generate.generate if notes else generate.template
        result = make(style, seed=self.seed.value(), bars=self.bars.value(), bpm=bpm, key=key)
        kind = "track" if notes else "template"
        title = f"{self.style_box.currentText()} {kind} (seed {self.seed.value()})"
        if not self.open_text(result.text, title):
            return None
        self.status.setText(
            f"{title}: {result.bpm:g} bpm, {result.key}. {result.notes[0] if result.notes else ''}"
        )
        return result
