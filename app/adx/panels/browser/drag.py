"""The browser's tree as a drag source: an entry with a payload drags it (dnd.py)."""

from __future__ import annotations

from collections.abc import Sequence

from PySide6.QtCore import QMimeData, Qt
from PySide6.QtWidgets import QTreeWidget, QTreeWidgetItem

from adx import dnd
from adx.panels.browser.model import Entry

ENTRY_ROLE = int(Qt.ItemDataRole.UserRole) + 1


class EntryTree(QTreeWidget):
    """A tree of Entry rows that drags their payloads."""

    def __init__(self) -> None:
        super().__init__()
        self.setHeaderHidden(True)
        self.setDragEnabled(True)
        self.setDragDropMode(QTreeWidget.DragDropMode.DragOnly)

    def add_entries(
        self, parent: QTreeWidgetItem | QTreeWidget, entries: tuple[Entry, ...]
    ) -> None:
        """Rows for ``entries`` under ``parent``."""
        for entry in entries:
            item = QTreeWidgetItem(parent)
            item.setText(0, entry.label)
            item.setData(0, ENTRY_ROLE, entry)
            if entry.path:
                item.setToolTip(0, entry.path)
            if entry.payload is None:
                item.setFlags(item.flags() & ~Qt.ItemFlag.ItemIsDragEnabled)
            if entry.children:
                self.add_entries(item, entry.children)

    @staticmethod
    def entry(item: QTreeWidgetItem | None) -> Entry | None:
        """The Entry a row shows."""
        if item is None:
            return None
        value = item.data(0, ENTRY_ROLE)
        return value if isinstance(value, Entry) else None

    def mimeData(self, items: Sequence[QTreeWidgetItem]) -> QMimeData:  # noqa: N802 - Qt's name
        entry = self.entry(items[0]) if items else None
        if entry is None or entry.payload is None:
            return QMimeData()
        return dnd.encode(entry.payload)

    def mimeTypes(self) -> list[str]:  # noqa: N802 - Qt's name
        return [dnd.MIME, "text/uri-list"]
