"""The playlist's toolbar: tools, snap, the pattern the draw tool places, arrangements
(named playlist states switched by one command, §4.1), and a new track."""

from __future__ import annotations

from typing import TYPE_CHECKING

from PySide6.QtWidgets import QComboBox, QInputDialog, QToolBar

from adx.engine_bridge import PPQ, Project

if TYPE_CHECKING:
    from adx.panels.playlist.panel import PlaylistPanel

SNAPS = (
    ("Bar", PPQ * 4),
    ("Beat", PPQ),
    ("1/2 beat", PPQ // 2),
    ("1/4 beat", PPQ // 4),
    ("None", 0),
)
#: The live arrangement's name until it has one.
LIVE = "Main"


class PlaylistToolbar(QToolBar):
    """Owned by the panel; holds only what is chosen in it (view state)."""

    def __init__(self, panel: PlaylistPanel) -> None:
        super().__init__()
        self.panel = panel
        for action in panel.tool_actions():
            self.addAction(action)
        self.addSeparator()
        self.snap = QComboBox()
        self.snap.addItems([label for label, _t in SNAPS])
        self.snap.setCurrentIndex(1)
        self.snap.setToolTip("Snap")
        self.addWidget(self.snap)
        self.pattern = QComboBox()
        self.pattern.setToolTip("The pattern Draw and Paint place")
        self.pattern.setMinimumWidth(110)
        self.addWidget(self.pattern)
        self.addSeparator()
        self.arrangement = QComboBox()
        self.arrangement.setToolTip("Arrangement: switching is one undoable command")
        self.arrangement.activated.connect(self._switch)
        self.addWidget(self.arrangement)
        self.addAction("+ Arrangement").triggered.connect(self.add_arrangement)
        self.addAction("Rename").triggered.connect(self._rename)
        self.addSeparator()
        self.addAction("+ Track").triggered.connect(
            lambda: panel.session.build_and_execute("AddPlaylistTrackNew", "")
        )
        follow = self.addAction("Follow")
        follow.setCheckable(True)
        follow.setToolTip("Scroll with the playhead")
        follow.toggled.connect(lambda on: setattr(panel, "follow", on))
        self._patterns: list[int] = []

    def refresh_choices(self, project: Project) -> None:
        """Re-read the patterns and arrangements the combos offer."""
        patterns = project.patterns_info()
        ids = [int(p["id"]) for p in patterns]
        if ids != self._patterns:
            current = self.pattern_id()
            self.pattern.clear()
            for p in patterns:
                self.pattern.addItem(str(p["name"]))
            self._patterns = ids
            if current in ids:
                self.pattern.setCurrentIndex(ids.index(current))
        info = project.playlist_info()
        names = [str(info["name"]) or LIVE, *[str(n) for n in info["arrangements"]]]
        if [self.arrangement.itemText(i) for i in range(self.arrangement.count())] != names:
            self.arrangement.clear()
            self.arrangement.addItems(names)
        self.arrangement.setCurrentIndex(0)

    def snap_ticks(self) -> int:
        """The snap grid, 0 for none."""
        return SNAPS[max(0, self.snap.currentIndex())][1]

    def pattern_id(self) -> int:
        """The draw pattern's id, 0 for none."""
        index = self.pattern.currentIndex()
        return self._patterns[index] if 0 <= index < len(self._patterns) else 0

    def choose_pattern(self, pattern: int) -> None:
        """Make ``pattern`` the one Draw places."""
        if pattern in self._patterns:
            self.pattern.setCurrentIndex(self._patterns.index(pattern))

    def _switch(self, index: int) -> None:
        # Index 0 is the live arrangement; stored ones follow, in order.
        if index > 0:
            self.panel.session.build_and_execute("SwitchArrangement", index - 1)

    def add_arrangement(self, name: str = "") -> None:
        """Store the live arrangement as a new named one, to switch between."""
        if not name:
            count = self.arrangement.count() + 1
            name, ok = QInputDialog.getText(self, "New arrangement", "Name", text=f"Alt{count}")
            if not ok or not name.strip():
                return
        session = self.panel.session
        with session.group("Add arrangement"):
            if not str(session.require().playlist_info()["name"]):
                session.build_and_execute("RenameArrangement", None, LIVE)
            session.build_and_execute("AddArrangement", name.strip(), True)

    def _rename(self) -> None:
        index = self.arrangement.currentIndex()
        name, ok = QInputDialog.getText(
            self, "Rename arrangement", "Name", text=self.arrangement.currentText()
        )
        if ok and name.strip():
            target = None if index <= 0 else index - 1
            self.panel.session.build_and_execute("RenameArrangement", target, name.strip())
