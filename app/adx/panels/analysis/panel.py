"""The Analysis panel (phase_10.md 2, 4.1): key and chords, timbre, cleanup and - in ONNX
builds - melody, each a tab over its own job."""

from __future__ import annotations

from PySide6.QtCore import QCoreApplication, QElapsedTimer, Qt
from PySide6.QtWidgets import QTabWidget, QWidget

from adx import engine_bridge
from adx.panels.analysis.cleanup import CleanupTab
from adx.panels.analysis.common import AnalysisTab
from adx.panels.analysis.key_chord import KeyChordTab
from adx.panels.analysis.melody import MelodyTab
from adx.panels.analysis.timbre import TimbreTab
from adx.panels.base import DockPanel
from adx.session import Session


class AnalysisPanel(DockPanel):
    panel_id = "analysis"
    title = "Analysis"
    scrolls = True
    default_area = Qt.DockWidgetArea.RightDockWidgetArea

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.tabs = QTabWidget()
        self.tabs.setObjectName("analysis.tabs")
        self.key_chord = KeyChordTab(session)
        self.timbre = TimbreTab(session)
        self.cleanup = CleanupTab(session)
        self.tabs.addTab(self.key_chord, "Key && chords")
        self.tabs.addTab(self.timbre, "Timbre")
        self.tabs.addTab(self.cleanup, "Clean up")
        self.melody: MelodyTab | None = None
        if engine_bridge.melody_extractor_available():
            self.melody = MelodyTab(session)
            self.tabs.addTab(self.melody, "Melody")
        self.setWidget(self.tabs)

    def melody_tab_shown(self) -> bool:
        return self.melody is not None and self.tabs.indexOf(self.melody) >= 0

    def all_tabs(self) -> list[AnalysisTab]:
        tabs: list[AnalysisTab] = [self.key_chord, self.timbre, self.cleanup]
        if self.melody is not None:
            tabs.append(self.melody)
        return tabs

    def wait_idle(self, timeout_ms: int = 60000) -> bool:
        """Spin the event loop until no tab's job is running (tests and the driver)."""
        timer = QElapsedTimer()
        timer.start()
        while any(t.runner.running for t in self.all_tabs()):
            if timer.elapsed() > timeout_ms:
                return False
            QCoreApplication.processEvents()
        return True
