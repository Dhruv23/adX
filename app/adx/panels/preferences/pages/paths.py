"""Paths: where samples, presets, projects and (Phase 9) plugins are found."""

from __future__ import annotations

from PySide6.QtWidgets import QFormLayout, QLineEdit, QWidget

from adx.preferences import Preferences


def _split(text: str) -> list[str]:
    return [part.strip() for part in text.split(";") if part.strip()]


class PathsPage(QWidget):
    """Folders, ``;``-separated where there may be several."""

    title = "Paths"

    def __init__(self, prefs: Preferences, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.prefs = prefs
        form = QFormLayout(self)
        self.samples = QLineEdit("; ".join(prefs.sample_dirs))
        form.addRow("Samples", self.samples)
        self.presets = QLineEdit("; ".join(prefs.preset_dirs))
        form.addRow("Presets", self.presets)
        self.projects = QLineEdit(prefs.project_dir)
        form.addRow("Projects", self.projects)
        self.plugins = QLineEdit("; ".join(prefs.plugin_dirs))
        self.plugins.setToolTip("Plugins arrive in Phase 9")
        form.addRow("Plugins", self.plugins)

    def apply(self) -> None:
        """Write this page's fields."""
        self.prefs.sample_dirs = _split(self.samples.text())
        self.prefs.preset_dirs = _split(self.presets.text())
        self.prefs.project_dir = self.projects.text().strip()
        self.prefs.plugin_dirs = _split(self.plugins.text())
