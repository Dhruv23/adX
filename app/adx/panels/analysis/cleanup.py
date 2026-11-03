"""Clean and structurize (phase_10.md 4.3): the form of one channel's part, rebuilt from
its best instance of each section, and markers where the sections start."""

from __future__ import annotations

from typing import Any

from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFormLayout,
    QLabel,
    QPushButton,
    QSpinBox,
    QWidget,
)

from adx import analysis, engine_bridge
from adx.panels.analysis.common import AnalysisTab
from adx.session import Session


class CleanupTab(AnalysisTab):
    name = "analysis.cleanup"

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(session, parent)
        grid = QFormLayout()
        self.target = QComboBox()
        self.target.setObjectName(f"{self.name}.target")
        self.phrase = QSpinBox()
        self.phrase.setObjectName(f"{self.name}.phrase")
        self.phrase.setRange(1, 32)
        self.phrase.setValue(4)
        self.phrase.setSuffix(" bars")
        grid.addRow("Part", self.target)
        grid.addRow("Phrase", self.phrase)
        self.form.addLayout(grid)
        self.summary = QLabel("")
        self.summary.setObjectName(f"{self.name}.summary")
        self.summary.setWordWrap(True)
        self.form.addWidget(self.summary)
        self.rebuild = QCheckBox("Rebuild the notes from the form")
        self.rebuild.setObjectName(f"{self.name}.rebuild")
        self.rebuild.setChecked(True)
        self.markers = QCheckBox("Add a marker at each section")
        self.markers.setObjectName(f"{self.name}.markers")
        self.markers.setChecked(True)
        self.form.addWidget(self.rebuild)
        self.form.addWidget(self.markers)
        self.apply_button = QPushButton("Apply")
        self.apply_button.setObjectName(f"{self.name}.apply")
        self.apply_button.setEnabled(False)
        self.apply_button.clicked.connect(self.apply)
        self.form.addWidget(self.apply_button)
        session.changed.connect(self._refresh)
        self._refresh()

    def _refresh(self) -> None:
        project = self.session.project
        current = self.target.currentText()
        self.target.clear()
        if project is None:
            return
        for pattern in project.patterns():
            for channel in project.pattern_channels(pattern):
                self.target.addItem(f"{pattern} / {channel}")
        if current:
            self.target.setCurrentText(current)

    def _part(self) -> tuple[str, str]:
        text = self.target.currentText()
        if " / " not in text:
            raise ValueError("pick a pattern and channel with notes")
        pattern, channel = text.split(" / ", 1)
        return pattern, channel

    def make_job(self) -> engine_bridge.AnalysisJob:
        project = self.session.project
        if project is None:
            raise ValueError("no project is open")
        pattern, channel = self._part()
        self.apply_button.setEnabled(False)
        self._target = (pattern, channel)
        return analysis.form_job(project, pattern, channel, phrase_bars=self.phrase.value())

    def show_result(self, result: dict[str, Any]) -> None:
        recovered = f"; follows {result['target']}" if result["used_blueprint"] else ""
        self.summary.setText(f"Form {result['form']}: {result['name']}{recovered}.")
        self.apply_button.setEnabled(True)

    def apply(self) -> None:
        if self.result is None:
            return
        pattern, channel = self._target
        analysis.apply_form(
            self.session,
            pattern,
            channel,
            self.result,
            rebuild=self.rebuild.isChecked(),
            markers=self.markers.isChecked(),
        )
        self.status.setText("Applied. Undo puts it back as it was.")
        self.apply_button.setEnabled(False)
