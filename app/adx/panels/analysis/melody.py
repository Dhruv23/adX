"""Audio to notes (phase_10.md 4.2). Only in builds with ADX_ENABLE_ONNX: without it the
panel does not create this tab at all, rather than show a feature that cannot run."""

from __future__ import annotations

from typing import Any

from PySide6.QtWidgets import QFormLayout, QLabel, QLineEdit, QPushButton, QWidget

from adx import analysis, engine_bridge
from adx.panels.analysis.common import AnalysisTab, FilePicker
from adx.session import Session


class MelodyTab(AnalysisTab):
    name = "analysis.melody"

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(session, parent)
        grid = QFormLayout()
        self.file = FilePicker(f"{self.name}.file")
        self.part_name = QLineEdit("Melody")
        self.part_name.setObjectName(f"{self.name}.name")
        grid.addRow("Audio", self.file)
        grid.addRow("Name", self.part_name)
        self.form.addLayout(grid)
        self.summary = QLabel("")
        self.summary.setObjectName(f"{self.name}.summary")
        self.form.addWidget(self.summary)
        self.apply_button = QPushButton("Add as a pattern")
        self.apply_button.setObjectName(f"{self.name}.apply")
        self.apply_button.setEnabled(False)
        self.apply_button.clicked.connect(self.apply)
        self.form.addWidget(self.apply_button)

    def make_job(self) -> engine_bridge.AnalysisJob:
        self.apply_button.setEnabled(False)
        return analysis.melody_job(self.file.text())

    def show_result(self, result: dict[str, Any]) -> None:
        self.summary.setText(f"{len(result['notes'])} notes.")
        self.apply_button.setEnabled(bool(result["notes"]))

    def apply(self) -> None:
        if self.result is None:
            return
        channel, pattern = analysis.apply_melody(
            self.session, self.result, self.part_name.text() or "Melody"
        )
        self.status.setText(f"Added {pattern} on channel {channel}.")
        self.apply_button.setEnabled(False)
