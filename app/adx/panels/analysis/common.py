"""What every analysis tab has: a run row (Run, Cancel, progress, a status line) over a
:class:`JobRunner`, and a file picker for the analyses that read audio."""

from __future__ import annotations

from typing import Any

from PySide6.QtWidgets import (
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QProgressBar,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from adx import engine_bridge
from adx.panels.analysis.runner import JobRunner
from adx.session import Session

AUDIO_FILTER = "Audio (*.wav *.flac *.mp3 *.ogg *.aif *.aiff)"


class FilePicker(QWidget):
    """A path field and a Browse button."""

    def __init__(self, name: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        self.path = QLineEdit()
        self.path.setObjectName(f"{name}.path")
        self.path.setPlaceholderText("an audio file")
        self.browse = QPushButton("Browse...")
        self.browse.setObjectName(f"{name}.browse")
        self.browse.clicked.connect(self._browse)
        row.addWidget(self.path, 1)
        row.addWidget(self.browse)

    def _browse(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Audio file", self.path.text(), AUDIO_FILTER)
        if path:
            self.path.setText(path)

    def text(self) -> str:
        return self.path.text().strip()


class AnalysisTab(QWidget):
    """A tab with a run row at the bottom. Subclasses build ``self.form`` and implement
    :meth:`make_job` and :meth:`show_result`."""

    #: The objectName prefix of this tab's widgets.
    name = "analysis"

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName(self.name)
        self.session = session
        self.runner = JobRunner(self)
        self.result: dict[str, Any] | None = None
        layout = QVBoxLayout(self)
        self.form = QVBoxLayout()
        layout.addLayout(self.form)
        row = QHBoxLayout()
        self.run_button = QPushButton("Analyze")
        self.run_button.setObjectName(f"{self.name}.run")
        self.run_button.clicked.connect(self.run)
        self.cancel_button = QPushButton("Cancel")
        self.cancel_button.setObjectName(f"{self.name}.cancel")
        self.cancel_button.setEnabled(False)
        self.cancel_button.clicked.connect(self.runner.cancel)
        self.bar = QProgressBar()
        self.bar.setObjectName(f"{self.name}.progress")
        self.bar.setRange(0, 1000)
        row.addWidget(self.run_button)
        row.addWidget(self.cancel_button)
        row.addWidget(self.bar, 1)
        layout.addLayout(row)
        self.status = QLabel("")
        self.status.setObjectName(f"{self.name}.status")
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        layout.addStretch(1)
        self.runner.progress.connect(lambda f: self.bar.setValue(int(f * 1000)))
        self.runner.finished.connect(self._finished)

    def make_job(self) -> engine_bridge.AnalysisJob:
        raise NotImplementedError

    def show_result(self, result: dict[str, Any]) -> None:
        raise NotImplementedError

    def run(self) -> None:
        try:
            job = self.make_job()
        except ValueError as error:
            self.status.setText(str(error))
            return
        self.result = None
        self.status.setText("Analyzing...")
        self.run_button.setEnabled(False)
        self.cancel_button.setEnabled(True)
        self.runner.start(job)

    def _finished(self, result: dict[str, Any]) -> None:
        self.run_button.setEnabled(True)
        self.cancel_button.setEnabled(False)
        if result.get("cancelled"):
            self.status.setText("Cancelled. Nothing was changed.")
            return
        if not result.get("ok"):
            self.status.setText(f"Failed: {result.get('error')}")
            return
        self.result = result
        self.status.setText("")
        self.show_result(result)
