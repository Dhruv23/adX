"""The base of every dockable panel (phase_5.md 4.10, 8).

A panel is a ``QDockWidget`` with a stable object name - which is what
``QMainWindow.saveState`` keys the layout on - and two hooks for its own UI state
(the piano roll's tool and grid, say), stored under the panel's group in QSettings.
Phase 6 adds a panel by deriving from this.
"""

from __future__ import annotations

from typing import ClassVar

from PySide6.QtCore import QSettings, Qt
from PySide6.QtWidgets import QDockWidget, QFrame, QScrollArea, QWidget


class DockPanel(QDockWidget):
    """A dockable panel with a stable identity and persisted UI state."""

    #: Stable across releases: saved layouts refer to it.
    panel_id: ClassVar[str] = "panel"
    title: ClassVar[str] = "Panel"
    default_area: ClassVar[Qt.DockWidgetArea] = Qt.DockWidgetArea.RightDockWidgetArea
    #: Form-like panels (editors full of controls) scroll rather than insist on their
    #: full height: a panel's minimum size pins the splitter beside it, and the
    #: instrument editor's 357 px minimum once kept the piano roll from growing.
    scrolls: ClassVar[bool] = False

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(self.title, parent)
        self.setObjectName(self.panel_id)
        self.setFeatures(
            QDockWidget.DockWidgetFeature.DockWidgetMovable
            | QDockWidget.DockWidgetFeature.DockWidgetFloatable
            | QDockWidget.DockWidgetFeature.DockWidgetClosable
        )

    def setWidget(self, widget: QWidget) -> None:  # noqa: N802 - Qt's name
        """Install the panel's body - inside a scroll area when :attr:`scrolls`."""
        if not self.scrolls:
            super().setWidget(widget)
            return
        area = QScrollArea()
        area.setWidgetResizable(True)
        area.setFrameShape(QFrame.Shape.NoFrame)
        area.setWidget(widget)
        super().setWidget(area)

    def body(self) -> QWidget | None:
        """The panel's own widget, inside the scroll area if there is one."""
        widget = self.widget()
        if isinstance(widget, QScrollArea) and self.scrolls:
            return widget.widget()
        return widget

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
