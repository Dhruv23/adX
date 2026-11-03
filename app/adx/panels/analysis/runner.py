"""One running analysis, polled from a timer (phase_10.md 4.1).

The job runs on its own engine thread; this only watches it: ``progress(fraction)``
while it runs, ``finished(result)`` once. The UI never waits on it.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QObject, QTimer, Signal

from adx import engine_bridge


class JobRunner(QObject):
    """At most one job at a time; starting another cancels the first."""

    progress = Signal(float)
    finished = Signal(object)

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self.job: engine_bridge.AnalysisJob | None = None
        self.last: dict[str, Any] | None = None
        self._timer = QTimer(self)
        self._timer.setInterval(50)
        self._timer.timeout.connect(self._poll)

    @property
    def running(self) -> bool:
        return self.job is not None

    def start(self, job: engine_bridge.AnalysisJob) -> None:
        self.cancel()
        self.job = job
        self.last = None
        self._timer.start()

    def cancel(self) -> None:
        if self.job is not None:
            self.job.cancel()
            self._finish()

    def _poll(self) -> None:
        if self.job is None:
            self._timer.stop()
            return
        if not self.job.done:
            self.progress.emit(self.job.progress)
            return
        self._finish()

    def _finish(self) -> None:
        job, self.job = self.job, None
        self._timer.stop()
        if job is None:
            return
        self.last = job.result()
        self.progress.emit(1.0)
        self.finished.emit(self.last)
