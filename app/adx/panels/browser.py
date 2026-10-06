"""The file browser (phase_5.md 2; its content - presets, samples, tagging - is
Phase 6's). A tree of projects and audio files: activating a project opens it,
activating an audio file shows it in the waveform panel."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QDir, QModelIndex, Qt, Signal
from PySide6.QtWidgets import QFileSystemModel, QTreeView, QWidget

from adx.panels.base import DockPanel

PROJECT_SUFFIXES = (".adx",)
AUDIO_SUFFIXES = (".wav", ".flac", ".mp3", ".ogg", ".aif", ".aiff")


class BrowserPanel(DockPanel):
    """Projects and audio under a root folder."""

    panel_id = "browser"
    title = "Browser"
    default_area = Qt.DockWidgetArea.LeftDockWidgetArea

    project_activated = Signal(str)
    audio_activated = Signal(str)

    def __init__(self, root: str = "", parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.model = QFileSystemModel(self)
        self.model.setNameFilters([f"*{s}" for s in (*PROJECT_SUFFIXES, *AUDIO_SUFFIXES)])
        self.model.setNameFilterDisables(False)
        self.model.setFilter(QDir.Filter.AllDirs | QDir.Filter.Files | QDir.Filter.NoDotAndDotDot)
        self.tree = QTreeView()
        self.tree.setModel(self.model)
        for column in (1, 2, 3):
            self.tree.hideColumn(column)
        self.tree.setHeaderHidden(True)
        self.tree.activated.connect(self._activated)
        self.setWidget(self.tree)
        self.set_root(root or str(Path.cwd()))

    def set_root(self, path: str) -> None:
        """Browse from ``path``."""
        index = self.model.setRootPath(path)
        self.tree.setRootIndex(index)

    def root(self) -> str:
        """The folder being browsed."""
        return self.model.rootPath()

    def _activated(self, index: QModelIndex) -> None:
        path = self.model.filePath(index)
        suffix = Path(path).suffix.lower()
        if suffix in PROJECT_SUFFIXES:
            self.project_activated.emit(path)
        elif suffix in AUDIO_SUFFIXES:
            self.audio_activated.emit(path)
