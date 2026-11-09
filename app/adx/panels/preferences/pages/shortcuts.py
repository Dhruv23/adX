"""Shortcuts: every registered action's key, rebindable (phase_11.6.md 4.4).

A view over the registry and the keymap. A rebind goes through
``ActionRegistry.remap``, which the keymap persists at once, so a change here takes
effect immediately and stays even if the dialog is cancelled.
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QKeySequence
from PySide6.QtWidgets import (
    QHBoxLayout,
    QHeaderView,
    QKeySequenceEdit,
    QLabel,
    QLineEdit,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from adx.actions import ActionRegistry

COLUMNS = ("Action", "Where", "Shortcut", "Default", "", "")
#: Column indexes.
LABEL, SCOPE, KEY, DEFAULT, CONFLICT, RESET = range(6)


def scope_title(scope: str) -> str:
    """'piano_roll' -> 'Piano roll'; a global action shows as 'Everywhere'."""
    return scope.replace("_", " ").capitalize() if scope else "Everywhere"


class ShortcutsPage(QWidget):
    """One row per action: label, scope, the key in force, the default, a conflict mark
    and Reset; a search field and Reset all above."""

    title = "Shortcuts"

    def __init__(self, registry: ActionRegistry, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.registry = registry
        self.keymap = registry.keymap
        layout = QVBoxLayout(self)
        top = QHBoxLayout()
        self.search = QLineEdit()
        self.search.setObjectName("shortcuts.search")
        self.search.setPlaceholderText("Search actions…")
        self.search.textChanged.connect(self.filter)
        top.addWidget(self.search, 1)
        self.reset_all_button = QPushButton("Reset all")
        self.reset_all_button.setObjectName("shortcuts.reset_all")
        self.reset_all_button.clicked.connect(self.reset_all)
        top.addWidget(self.reset_all_button)
        layout.addLayout(top)
        layout.addWidget(QLabel("Changes apply at once. A key works only where its row says."))
        self.ids = [spec.id for spec in registry.specs()]
        self.table = QTableWidget(len(self.ids), len(COLUMNS))
        self.table.setObjectName("shortcuts.table")
        self.table.setHorizontalHeaderLabels(list(COLUMNS))
        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(LABEL, QHeaderView.ResizeMode.Stretch)
        self.edits: dict[str, QKeySequenceEdit] = {}
        self.marks: dict[str, QTableWidgetItem] = {}
        for row, action_id in enumerate(self.ids):
            spec = registry.spec(action_id)
            label = QTableWidgetItem(spec.label.replace("&", "").rstrip("…"))
            label.setToolTip(action_id)
            self.table.setItem(row, LABEL, label)
            self.table.setItem(row, SCOPE, QTableWidgetItem(scope_title(spec.scope)))
            edit = QKeySequenceEdit()
            edit.setObjectName(f"shortcuts.key.{action_id}")
            edit.setMaximumSequenceLength(1)
            edit.editingFinished.connect(lambda a=action_id: self._edited(a))
            self.table.setCellWidget(row, KEY, edit)
            self.edits[action_id] = edit
            default = QTableWidgetItem(self.keymap.default(action_id))
            default.setForeground(Qt.GlobalColor.gray)
            self.table.setItem(row, DEFAULT, default)
            mark = QTableWidgetItem("")
            self.table.setItem(row, CONFLICT, mark)
            self.marks[action_id] = mark
            reset = QPushButton("Reset")
            reset.clicked.connect(lambda _c=False, a=action_id: self.reset(a))
            self.table.setCellWidget(row, RESET, reset)
        layout.addWidget(self.table, 1)
        self.refresh()

    # --- the rows -------------------------------------------------------------------

    def refresh(self) -> None:
        """Every row from the keymap: the key in force and whether it clashes."""
        clashes: dict[str, list[str]] = {}
        for ids in self.keymap.conflicts().values():
            for action_id in ids:
                clashes[action_id] = [other for other in ids if other != action_id]
        for action_id in self.ids:
            edit = self.edits[action_id]
            edit.blockSignals(True)
            edit.setKeySequence(QKeySequence(self.keymap.shortcut(action_id)))
            edit.blockSignals(False)
            mark = self.marks[action_id]
            others = clashes.get(action_id, [])
            mark.setText("⚠ conflict" if others else "")
            mark.setToolTip(
                "Also bound to: " + ", ".join(self._label(o) for o in others) if others else ""
            )

    def _label(self, action_id: str) -> str:
        return self.registry.spec(action_id).label.replace("&", "").rstrip("…")

    def _edited(self, action_id: str) -> None:
        sequence = self.edits[action_id].keySequence()
        self.rebind(action_id, sequence.toString(QKeySequence.SequenceFormat.PortableText))

    def rebind(self, action_id: str, shortcut: str) -> None:
        """Give an action a key ("" clears it), through the registry."""
        self.registry.remap(action_id, shortcut)
        self.refresh()

    def reset(self, action_id: str) -> None:
        """One action back to the key it shipped with."""
        self.rebind(action_id, self.keymap.default(action_id))

    def reset_all(self) -> None:
        self.keymap.reset()
        for action_id in self.ids:
            self.registry.remap(action_id, self.keymap.default(action_id))
        self.refresh()

    def shown_shortcut(self, action_id: str) -> str:
        """What the row's key field shows, in portable text."""
        return (
            self.edits[action_id].keySequence().toString(QKeySequence.SequenceFormat.PortableText)
        )

    def conflict_marked(self, action_id: str) -> bool:
        return bool(self.marks[action_id].text())

    def filter(self, text: str) -> None:
        """Hide rows whose label, id or scope doesn't contain the text."""
        needle = text.strip().casefold()
        for row, action_id in enumerate(self.ids):
            spec = self.registry.spec(action_id)
            hay = f"{spec.label} {action_id} {scope_title(spec.scope)}".replace("&", "")
            self.table.setRowHidden(row, bool(needle) and needle not in hay.casefold())

    def visible_rows(self) -> int:
        return sum(not self.table.isRowHidden(r) for r in range(self.table.rowCount()))

    def apply(self) -> None:
        """Nothing to write: rebinds are already saved."""
