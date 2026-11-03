"""A hosted plugin's own editor, in a window of ours (phase_9.md 4.6).

The plugin's GUI lives in its sandbox process: the child makes a native window, the
plugin draws into it, and this window embeds it by handle (``QWindow.fromWinId``). A
crash takes the editor with the child, never this window.

Some plugins embed badly, and some have no editor at all. The generic editor - every
parameter from the plugin's own list - is always there, below, so such a plugin stays
usable rather than not.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import Qt
from PySide6.QtGui import QCloseEvent, QWindow
from PySide6.QtWidgets import QLabel, QVBoxLayout, QWidget

from adx.engine_bridge import Plugins


class PluginWindow(QWidget):
    """One instance's editor: embedded if it has one, the generic editor regardless."""

    def __init__(
        self,
        instance: int,
        title: str,
        generic: Callable[[], QWidget] | None = None,
        embed: bool = True,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent, Qt.WindowType.Window)
        self.instance = instance
        self.setWindowTitle(title)
        self.setObjectName(f"plugin-window:{instance}")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.native: QWindow | None = None
        self.container: QWidget | None = None
        opened = Plugins.open_editor(instance) if embed and instance else None
        if opened is not None:
            handle, width, height = opened
            self.native = QWindow.fromWinId(handle)
            self.container = QWidget.createWindowContainer(self.native, self)
            self.container.setMinimumSize(max(width, 100), max(height, 60))
            layout.addWidget(self.container, 1)
        else:
            note = QLabel("This plugin has no editor of its own here; its parameters are below.")
            note.setWordWrap(True)
            note.setMargin(8)
            layout.addWidget(note)
        self.generic = generic() if generic is not None else None
        if self.generic is not None:
            layout.addWidget(self.generic, 0 if self.container is not None else 1)
        if self.container is not None:
            self.resize(self.container.minimumWidth(), self.container.minimumHeight() + 240)
        else:
            self.resize(640, 420)

    @property
    def embedded(self) -> bool:
        """The plugin's own editor is showing (not only the generic one)."""
        return self.container is not None

    def closeEvent(self, event: QCloseEvent) -> None:  # noqa: N802 - Qt's name
        if self.instance and self.embedded:
            Plugins.close_editor(self.instance)
        super().closeEvent(event)
