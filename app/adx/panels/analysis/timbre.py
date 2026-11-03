"""Timbre from a mixdown (phase_10.md 4.3): the arrangement's notes say where a sound is
and at what pitch, the reference audio says what it sounds like there, and the result is
an additive channel playing the recovered patch."""

from __future__ import annotations

from typing import Any

from PySide6.QtWidgets import QComboBox, QFormLayout, QLabel, QLineEdit, QPushButton, QWidget

from adx import analysis, engine_bridge
from adx.panels.analysis.common import AnalysisTab, FilePicker
from adx.session import Session

ALL_CHANNELS = "All pitched channels"


class TimbreTab(AnalysisTab):
    name = "analysis.timbre"

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(session, parent)
        grid = QFormLayout()
        self.file = FilePicker(f"{self.name}.file")
        self.channel = QComboBox()
        self.channel.setObjectName(f"{self.name}.channel")
        self.channel.setToolTip("Whose notes sound in the reference")
        self.patch_name = QLineEdit("Analyzed")
        self.patch_name.setObjectName(f"{self.name}.name")
        grid.addRow("Reference", self.file)
        grid.addRow("Notes of", self.channel)
        grid.addRow("Name", self.patch_name)
        self.form.addLayout(grid)
        self.summary = QLabel("")
        self.summary.setObjectName(f"{self.name}.summary")
        self.summary.setWordWrap(True)
        self.form.addWidget(self.summary)
        self.apply_button = QPushButton("Add as a channel")
        self.apply_button.setObjectName(f"{self.name}.apply")
        self.apply_button.setEnabled(False)
        self.apply_button.clicked.connect(self.apply)
        self.form.addWidget(self.apply_button)
        session.changed.connect(self._refresh)
        self._refresh()

    def _refresh(self) -> None:
        project = self.session.project
        current = self.channel.currentText()
        self.channel.clear()
        self.channel.addItem(ALL_CHANNELS)
        if project is not None:
            self.channel.addItems(project.channels())
        if current:
            self.channel.setCurrentText(current)

    def make_job(self) -> engine_bridge.AnalysisJob:
        project = self.session.project
        if project is None:
            raise ValueError("no project is open")
        which = self.channel.currentText()
        notes = analysis.arrangement_notes(project, None if which == ALL_CHANNELS else [which])
        if not notes:
            raise ValueError("no notes to measure under")
        self.apply_button.setEnabled(False)
        return analysis.timbre_job(self.file.text(), notes, self.patch_name.text() or "Analyzed")

    def show_result(self, result: dict[str, Any]) -> None:
        strongest = sorted(
            ((level, k + 1) for k, level in enumerate(result["harmonics"]) if level > 0.0),
            reverse=True,
        )[:6]
        listed = ", ".join(f"h{k} {level:.2f}" for level, k in strongest)
        self.summary.setText(
            f"{result['notes_measured']} notes measured. Harmonics: {listed}. "
            f"Attack {result['attack'] * 1000:.0f} ms, sustain {result['sustain']:.2f}, "
            f"release {result['release'] * 1000:.0f} ms."
        )
        self.apply_button.setEnabled(True)

    def apply(self) -> None:
        if self.result is None:
            return
        name = analysis.apply_timbre(
            self.session, self.result, self.patch_name.text() or "Analyzed"
        )
        self.status.setText(f"Added channel {name}.")
        self.apply_button.setEnabled(False)
