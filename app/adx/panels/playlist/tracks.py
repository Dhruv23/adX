"""The playlist's track headers: name, colour, mute, solo, height, group collapse, and
the freeze placeholder (phase_6.md 4.1; Phase 8 makes freeze real).

A column of small widgets beside the clip view, one per visible track, each exactly its
track's height in pixels, scrolled with the view. Every value shown comes from a fresh
read in :meth:`TrackHeaders.show_tracks`; every edit is a signal.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QContextMenuEvent, QMouseEvent
from PySide6.QtWidgets import QHBoxLayout, QLabel, QMenu, QToolButton, QWidget

from adx.widgets.color_swatch import ColorSwatch
from adx.widgets.step_button import StepButton

HEADER_WIDTH = 190


class TrackHeader(QWidget):
    """One track's header."""

    changed = Signal(int, dict)
    renamed = Signal(int, str)

    def __init__(self, track: int, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.track = track
        self._height = 1
        layout = QHBoxLayout(self)
        layout.setContentsMargins(3, 1, 3, 1)
        layout.setSpacing(3)
        self.collapse = QToolButton()
        self.collapse.setFixedSize(16, 16)
        self.collapse.setToolTip("Collapse or expand this group")
        self.collapse.clicked.connect(self._toggle_collapse)
        self.swatch = ColorSwatch()
        self.swatch.color_chosen.connect(lambda c: self.changed.emit(self.track, {"color": c}))
        self.name = QLabel()
        self.name.setMinimumWidth(60)
        self.mute = StepButton("M", "Mute track")
        self.mute.toggled_by_user.connect(lambda on: self.changed.emit(self.track, {"muted": on}))
        self.solo = StepButton("S", "Solo track")
        self.solo.toggled_by_user.connect(lambda on: self.changed.emit(self.track, {"soloed": on}))
        self.freeze = QToolButton()
        self.freeze.setText("❄")
        self.freeze.setEnabled(False)
        self.freeze.setToolTip("Freeze arrives in Phase 8 (plans/phase_8.md)")
        for widget in (self.collapse, self.swatch, self.name, self.mute, self.solo, self.freeze):
            layout.addWidget(widget)
        self._collapsed = False

    def show_track(self, info: dict[str, Any], is_group: bool) -> None:
        """Display a fresh read. Emits nothing."""
        indent = "    " if int(info["group"]) else ""
        self.name.setText(indent + (str(info["name"]) or f"Track {info['id']}"))
        self.swatch.set_color(str(info["color"]))
        self.mute.set_checked(bool(info["muted"]))
        self.solo.set_checked(bool(info["soloed"]))
        self._collapsed = bool(info["collapsed"])
        self._height = int(info["height"])
        self.collapse.setVisible(is_group)
        self.collapse.setText("▸" if self._collapsed else "▾")
        self.name.setEnabled(bool(info["audible"]))

    def _toggle_collapse(self) -> None:
        self.changed.emit(self.track, {"collapsed": not self._collapsed})

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        from PySide6.QtWidgets import QInputDialog

        name, ok = QInputDialog.getText(self, "Rename track", "Name", text=self.name.text().strip())
        if ok:
            self.changed.emit(self.track, {"name": name.strip()})

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:  # noqa: N802 - Qt's name
        menu = QMenu(self)
        taller = menu.addAction("Taller")
        shorter = menu.addAction("Shorter")
        ungroup = menu.addAction("Leave group")
        chosen = menu.exec(event.globalPos())
        if chosen is taller:
            self.changed.emit(self.track, {"height": min(64, self._height + 1)})
        elif chosen is shorter:
            self.changed.emit(self.track, {"height": max(1, self._height - 1)})
        elif chosen is ungroup:
            self.changed.emit(self.track, {"group": 0})


class TrackHeaders(QWidget):
    """The column of headers, positioned to match the clip view's rows."""

    changed = Signal(int, dict)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setFixedWidth(HEADER_WIDTH)
        self.headers: dict[int, TrackHeader] = {}
        self._rows: list[tuple[int, float, float]] = []
        self._row_start = 0.0
        self._pixels_per_row = 44.0

    def show_tracks(
        self,
        tracks: list[dict[str, Any]],
        rows: list[tuple[int, float, float]],
        row_start: float,
        pixels_per_row: float,
    ) -> None:
        """Rebuild the headers from a fresh read and the builder's row layout."""
        ids = [int(t["id"]) for t in tracks]
        for track in list(self.headers):
            if track not in ids:
                self.headers.pop(track).deleteLater()
        groups = {int(t["group"]) for t in tracks if int(t["group"])}
        for info in tracks:
            track = int(info["id"])
            header = self.headers.get(track)
            if header is None:
                header = TrackHeader(track, self)
                header.changed.connect(self.changed)
                self.headers[track] = header
                header.show()
            header.show_track(info, track in groups)
        self._rows = rows
        self.place(row_start, pixels_per_row)

    def place(self, row_start: float, pixels_per_row: float) -> None:
        """Move every header to its row (a scroll or a zoom: no reads)."""
        self._row_start, self._pixels_per_row = row_start, pixels_per_row
        for track, top, height in self._rows:
            header = self.headers.get(track)
            if header is None:
                continue
            header.setVisible(height > 0)
            header.setGeometry(
                0,
                round((top - row_start) * pixels_per_row),
                self.width(),
                max(1, round(height * pixels_per_row) - 1),
            )

    def header_alignment(self) -> Qt.AlignmentFlag:
        """For layouts: headers hang from the top."""
        return Qt.AlignmentFlag.AlignTop
