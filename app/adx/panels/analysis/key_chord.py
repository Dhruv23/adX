"""Key and chords (phase_10.md 3 step 2): of the arrangement's notes or of an audio file,
with what might come next. The key is shown with its confidence and runners-up, since
an honest 70 % is a suggestion, not an assertion (phase_10.md 10)."""

from __future__ import annotations

from typing import Any

from PySide6.QtWidgets import (
    QComboBox,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QPushButton,
    QWidget,
)

from adx import analysis, engine_bridge
from adx.engine_bridge import PPQ
from adx.panels.analysis.common import AnalysisTab, FilePicker
from adx.session import Session

PROJECT_NOTES = "The arrangement's notes"
AUDIO_FILE = "An audio file"


def _clock(seconds: float) -> str:
    minutes, rest = divmod(max(0.0, seconds), 60.0)
    return f"{int(minutes)}:{rest:05.2f}"


class KeyChordTab(AnalysisTab):
    name = "analysis.key"

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(session, parent)
        grid = QFormLayout()
        self.source = QComboBox()
        self.source.setObjectName(f"{self.name}.source")
        self.source.addItems([PROJECT_NOTES, AUDIO_FILE])
        self.file = FilePicker(f"{self.name}.file")
        self.source.currentTextChanged.connect(lambda t: self.file.setEnabled(t == AUDIO_FILE))
        self.file.setEnabled(False)
        grid.addRow("Source", self.source)
        grid.addRow("File", self.file)
        self.key_label = QLabel("-")
        self.key_label.setObjectName(f"{self.name}.key")
        self.confidence = QLabel("")
        self.confidence.setObjectName(f"{self.name}.confidence")
        grid.addRow("Key", self.key_label)
        grid.addRow("", self.confidence)
        self.form.addLayout(grid)
        self.form.addWidget(QLabel("Chords"))
        self.chord_list = QListWidget()
        self.chord_list.setObjectName(f"{self.name}.chords")
        self.form.addWidget(self.chord_list, 1)
        self.form.addWidget(QLabel("What might come next"))
        self.suggestions = QListWidget()
        self.suggestions.setObjectName(f"{self.name}.suggestions")
        self.form.addWidget(self.suggestions, 1)
        add = QHBoxLayout()
        self.target = QComboBox()
        self.target.setObjectName(f"{self.name}.target")
        self.target.setToolTip("The pattern and channel a suggested chord is written into")
        self.add_chord = QPushButton("Add chord at bar")
        self.add_chord.setObjectName(f"{self.name}.add")
        self.add_chord.clicked.connect(self.insert_suggestion)
        add.addWidget(self.target, 1)
        add.addWidget(self.add_chord)
        self.form.addLayout(add)
        session.changed.connect(self._refresh_targets)
        self._refresh_targets()

    def use_project_notes(self) -> None:
        self.source.setCurrentText(PROJECT_NOTES)

    def use_file(self, path: str) -> None:
        self.source.setCurrentText(AUDIO_FILE)
        self.file.path.setText(path)

    def make_job(self) -> engine_bridge.AnalysisJob:
        if self.source.currentText() == AUDIO_FILE:
            return analysis.chords_job(path=self.file.text())
        project = self.session.project
        if project is None:
            raise ValueError("no project is open")
        notes = analysis.arrangement_notes(project)
        if not notes:
            raise ValueError("the arrangement plays no pitched notes")
        return analysis.chords_job(notes=notes, frame_seconds=0.5)

    def show_result(self, result: dict[str, Any]) -> None:
        key = result["key"]
        self.key_label.setText(str(key["name"]))
        runners = ", ".join(f"{name} {score:.2f}" for name, score in key["ranked"][1:4])
        self.confidence.setText(f"confidence {key['confidence']:.2f}; then {runners}")
        self.chord_list.clear()
        for seg in result["segments"]:
            roman = f"  ({seg['roman']})" if seg["roman"] else ""
            self.chord_list.addItem(
                f"{seg['chord']}{roman}  {_clock(seg['start'])}-{_clock(seg['end'])}"
            )
        self.suggestions.clear()
        self._suggested = list(result["suggestions"])
        for s in self._suggested:
            self.suggestions.addItem(f"{s['chord']}  {s['roman']}  {s['score']:.2f}")

    def _refresh_targets(self) -> None:
        project = self.session.project
        current = self.target.currentText()
        self.target.clear()
        if project is None:
            return
        for pattern in project.patterns():
            for channel in project.pattern_channels(pattern) or project.channels():
                self.target.addItem(f"{pattern} / {channel}")
        if current:
            self.target.setCurrentText(current)

    def insert_suggestion(self) -> None:
        """The selected suggestion as a chord, one bar long, at the start of the bar after
        the target's last note: one undo step."""
        row = self.suggestions.currentRow()
        project = self.session.project
        if row < 0 or project is None or " / " not in self.target.currentText():
            self.status.setText("Pick a suggestion and a pattern first.")
            return
        pattern, channel = self.target.currentText().split(" / ", 1)
        notes = project.notes(pattern, channel)
        bar = PPQ * 4
        end = max((int(n["start"]) + int(n["length"]) for n in notes), default=0)
        at = -(-end // bar) * bar
        analysis.apply_chord(
            self.session, pattern, channel, list(self._suggested[row]["tones"]), at, bar
        )
        self.status.setText(f"Added {self._suggested[row]['chord']} at bar {at // bar + 1}.")
