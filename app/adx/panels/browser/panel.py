"""The browser (phase_6.md 4.6): samples, presets, patterns, effects, plugins and
favourites in one searchable tree. Drag anything with a payload onto a panel that takes
it; click an audio file to preview it; activate a project to open it."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QPoint, QSettings, Qt, Signal
from PySide6.QtWidgets import (
    QComboBox,
    QLineEdit,
    QMenu,
    QToolBar,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from adx import engine_bridge
from adx.panels.base import DockPanel
from adx.panels.browser.drag import EntryTree
from adx.panels.browser.model import (
    Entry,
    all_tags,
    effect_entries,
    filtered,
    folder_entries,
    pattern_entries,
    preset_entries,
)
from adx.panels.browser.preview import Preview
from adx.session import Session

#: Where the built-in preset packs live, beside the engine's sources.
PACKS = Path(__file__).resolve().parents[4] / "engine" / "preset" / "packs"


class BrowserPanel(DockPanel):
    """Everything that can be dragged into a project."""

    panel_id = "browser"
    title = "Browser"
    default_area = Qt.DockWidgetArea.LeftDockWidgetArea

    project_activated = Signal(str)
    audio_activated = Signal(str)

    def __init__(
        self, root: str = "", parent: QWidget | None = None, session: Session | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.preview = Preview(self)
        self.favorites: list[str] = []
        self.preset_dirs: list[str] = [str(PACKS)]
        self._root = root or str(Path.cwd())
        body = QWidget()
        layout = QVBoxLayout(body)
        layout.setContentsMargins(0, 0, 0, 0)
        bar = QToolBar()
        self.search = QLineEdit()
        self.search.setPlaceholderText("Search names and tags")
        self.search.textChanged.connect(lambda _t: self.refresh())
        bar.addWidget(self.search)
        self.tag = QComboBox()
        self.tag.setToolTip("Show presets with this tag")
        self.tag.activated.connect(lambda _i: self.refresh())
        bar.addWidget(self.tag)
        layout.addWidget(bar)
        self.tree = EntryTree()
        self.tree.itemClicked.connect(self._clicked)
        self.tree.itemActivated.connect(self._activated)
        self.tree.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.tree.customContextMenuRequested.connect(self._context_menu)
        layout.addWidget(self.tree, 1)
        self.setWidget(body)
        if session is not None:
            session.changed.connect(self.refresh)
        self.refresh()

    # --- reading --------------------------------------------------------------------

    def set_root(self, path: str) -> None:
        """Browse samples and projects from ``path``."""
        self._root = path
        self.refresh()

    def root(self) -> str:
        """The folder being browsed."""
        return self._root

    def sections(self) -> tuple[Entry, ...]:
        """Every section, read now."""
        presets = engine_bridge.scan_presets(self.preset_dirs)
        tags = all_tags(presets)
        if [self.tag.itemText(i) for i in range(1, self.tag.count())] != tags:
            current = self.tag.currentText()
            self.tag.clear()
            self.tag.addItem("")
            self.tag.addItems(tags)
            self.tag.setCurrentText(current)
        patterns = (
            pattern_entries(self.session.project.patterns_info())
            if self.session is not None and self.session.project is not None
            else ()
        )
        effects = [t for t in engine_bridge.type_catalog() if t["kind"] == "effect"]
        favorites = tuple(
            e
            for path in self.favorites
            for e in folder_entries(str(Path(path).parent))
            if e.path == path
        )
        return (
            Entry("Samples & projects", "folder", self._root, children=folder_entries(self._root)),
            Entry("Presets", "folder", children=preset_entries(presets, self.tag.currentText())),
            Entry("Patterns", "folder", children=patterns),
            Entry("Effects", "folder", children=effect_entries(effects)),
            Entry("Plugins", "folder", children=(Entry("Plugins arrive in Phase 9", "note"),)),
            Entry("Favorites", "folder", children=favorites),
        )

    def refresh(self) -> None:
        """Rebuild the tree from fresh reads, filtered by the search."""
        words = [w.lower() for w in self.search.text().split() if w]
        expanded = {item.text(0) for item in self._top_items() if item.isExpanded()}
        self.tree.clear()
        self.tree.add_entries(self.tree, filtered(self.sections(), words))
        for i, item in enumerate(self._top_items()):
            item.setExpanded(bool(words) or item.text(0) in expanded or i == 0)

    def _top_items(self) -> list[QTreeWidgetItem]:
        items = (self.tree.topLevelItem(i) for i in range(self.tree.topLevelItemCount()))
        return [item for item in items if item is not None]

    # --- interaction ----------------------------------------------------------------

    def _clicked(self, item: QTreeWidgetItem, _column: int) -> None:
        entry = self.tree.entry(item)
        if entry is not None and entry.kind == "audio":
            self.preview.play(entry.path)
            self.audio_activated.emit(entry.path)

    def _activated(self, item: QTreeWidgetItem, _column: int) -> None:
        entry = self.tree.entry(item)
        if entry is not None and entry.kind == "project":
            self.project_activated.emit(entry.path)

    def _context_menu(self, point: QPoint) -> None:
        item = self.tree.itemAt(point)
        entry = self.tree.entry(item)
        if entry is None or not entry.path:
            return
        menu = QMenu(self)
        if entry.path in self.favorites:
            action = menu.addAction("Remove from favourites")
            action.triggered.connect(lambda: self._favorite(entry.path, False))
        else:
            action = menu.addAction("Add to favourites")
            action.triggered.connect(lambda: self._favorite(entry.path, True))
        menu.exec(self.tree.viewport().mapToGlobal(point))

    def _favorite(self, path: str, add: bool) -> None:
        if add and path not in self.favorites:
            self.favorites.append(path)
        elif not add and path in self.favorites:
            self.favorites.remove(path)
        self.refresh()

    # --- state ----------------------------------------------------------------------

    def write_state(self, settings: QSettings) -> None:
        settings.setValue("favorites", self.favorites)

    def read_state(self, settings: QSettings) -> None:
        value = settings.value("favorites", [])
        self.favorites = (
            [str(v) for v in value] if isinstance(value, list) else ([str(value)] if value else [])
        )
        self.refresh()
