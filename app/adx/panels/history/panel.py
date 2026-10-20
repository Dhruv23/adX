"""The history panel (phase_6.md 4.8): every undo step, the current position marked, the
steps a redo would bring back greyed out below it. Click a row to jump there - undo or
redo until exactly that step is the last one done (``history_jump``).

Coalesced edits are one row (a drag is "Move clip", once), which is what keeps the list
readable rather than a wall of moves.
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QBrush, QColor
from PySide6.QtWidgets import QListWidget, QListWidgetItem, QWidget

from adx.panels.base import DockPanel
from adx.session import Session
from adx.theme.tokens import DARK, Theme

#: The row above the first step: the project as it was opened.
ORIGIN = "(opened)"


class HistoryPanel(DockPanel):
    """Undo history as a list."""

    panel_id = "history"
    title = "History"
    default_area = Qt.DockWidgetArea.LeftDockWidgetArea

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.session = session
        self.list = QListWidget()
        self.list.itemClicked.connect(self._clicked)
        self.setWidget(self.list)
        self._theme: Theme = DARK
        session.changed.connect(self.refresh)

    def refresh(self) -> None:
        """Re-read the history: done steps, the marker, then redoable ones."""
        if self.session.project is None:
            return
        done, redo = self.session.project.history_info()
        self.list.blockSignals(True)
        self.list.clear()
        rows = [ORIGIN, *done]
        for depth, label in enumerate(rows):
            item = QListWidgetItem(label)
            item.setData(Qt.ItemDataRole.UserRole, depth)
            if depth == len(done):
                item.setText(f"▶ {label}")
                font = item.font()
                font.setBold(True)
                item.setFont(font)
            self.list.addItem(item)
        for offset, label in enumerate(redo):
            item = QListWidgetItem(label)
            item.setData(Qt.ItemDataRole.UserRole, len(done) + offset + 1)
            item.setForeground(QBrush(QColor(self._theme.text_dim)))
            self.list.addItem(item)
        self.list.blockSignals(False)
        self.list.scrollToItem(self.list.item(len(done)))

    def jump(self, depth: int) -> bool:
        """Undo or redo so that ``depth`` steps are done."""
        return self.session.undo_to(depth)

    def _clicked(self, item: QListWidgetItem) -> None:
        self.jump(int(item.data(Qt.ItemDataRole.UserRole)))

    def apply_theme(self, theme: Theme) -> None:
        """Recolour the redo rows."""
        self._theme = theme
        self.refresh()
