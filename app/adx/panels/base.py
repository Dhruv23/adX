"""The base of every dockable panel (phase_5.md 4.10, 8).

A panel is a ``QDockWidget`` with a stable object name - which is what
``QMainWindow.saveState`` keys the layout on - and two hooks for its own UI state
(the piano roll's tool and grid, say), stored under the panel's group in QSettings.
Phase 6 adds a panel by deriving from this.
"""

from __future__ import annotations

from typing import ClassVar

from PySide6.QtCore import QSettings, Qt
from PySide6.QtWidgets import QDockWidget, QWidget


class DockPanel(QDockWidget):
    """A dockable panel with a stable identity and persisted UI state."""

    #: Stable across releases: saved layouts refer to it.
    panel_id: ClassVar[str] = "panel"
    title: ClassVar[str] = "Panel"
    default_area: ClassVar[Qt.DockWidgetArea] = Qt.DockWidgetArea.RightDockWidgetArea

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(self.title, parent)
        self.setObjectName(self.panel_id)
        self.setFeatures(
            QDockWidget.DockWidgetFeature.DockWidgetMovable
            | QDockWidget.DockWidgetFeature.DockWidgetFloatable
            | QDockWidget.DockWidgetFeature.DockWidgetClosable
        )

    def save_state(self, settings: QSettings) -> None:
        """Store this panel's own UI state (override; call within its group)."""
        settings.beginGroup(self.panel_id)
        self.write_state(settings)
        settings.endGroup()

    def restore_state(self, settings: QSettings) -> None:
        """Restore what :meth:`save_state` stored."""
        settings.beginGroup(self.panel_id)
        self.read_state(settings)
        settings.endGroup()

    def write_state(self, settings: QSettings) -> None:
        """Subclass hook: write keys into the panel's group."""

    def read_state(self, settings: QSettings) -> None:
        """Subclass hook: read keys from the panel's group. Must tolerate missing keys."""
