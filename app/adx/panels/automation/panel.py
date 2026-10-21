"""The automation panel: lanes and clip envelopes, and controllers (phase_6.md 4.5).

Left, every lane the project has - playlist automation clips, pattern lanes, and the
envelopes of the selected placement (labelled as clip envelopes: a clip envelope travels
with its clip, a playlist automation clip spans many). Right, the selected lane in the
lane editor, with the shape library and snapping. A second tab holds the controllers.
"""

from __future__ import annotations

from collections.abc import Callable

import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtGui import QAction
from PySide6.QtWidgets import (
    QCheckBox,
    QHBoxLayout,
    QListWidget,
    QMenu,
    QSplitter,
    QTabWidget,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from adx.panels.automation.controllers import ControllerEditor
from adx.panels.automation.lane_editor import LaneEditor, LaneSource, Points
from adx.panels.base import DockPanel
from adx.selection import Kind, Selection
from adx.session import Session
from adx.theme.tokens import Theme

SHAPES = ("ramp", "pulse", "sine", "random")


class AutomationPanel(DockPanel):
    """Lanes, envelopes and controllers."""

    panel_id = "automation"
    title = "Automation"
    default_area = Qt.DockWidgetArea.BottomDockWidgetArea
    scrolls = True

    def __init__(
        self, session: Session, selection: Selection, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.selection = selection
        self.editor = LaneEditor()
        tabs = QTabWidget()
        lanes = QWidget()
        layout = QHBoxLayout(lanes)
        split = QSplitter(Qt.Orientation.Horizontal)
        self.lanes = QListWidget()
        self.lanes.currentRowChanged.connect(lambda _r: self._lane_chosen())
        split.addWidget(self.lanes)
        right = QWidget()
        column = QVBoxLayout(right)
        column.setContentsMargins(0, 0, 0, 0)
        bar = QToolBar()
        shapes = QMenu("Shape", self)
        #: Each toolbar operation is one discrete step, however soon after the last.
        self.shape_actions: dict[str, QAction] = {}
        for shape in SHAPES:
            action = shapes.addAction(shape.title())
            action.triggered.connect(
                lambda _c=False, s=shape: self._discrete(self.editor.apply_shape, s)
            )
            self.shape_actions[shape] = action
        bar.addAction(shapes.menuAction())
        self.delete_action = bar.addAction("Delete points")
        self.delete_action.triggered.connect(lambda: self._discrete(self.editor.delete_selected))
        snap = QCheckBox("Snap values")
        snap.toggled.connect(lambda on: setattr(self.editor, "snap_values", on))
        bar.addWidget(snap)
        column.addWidget(bar)
        column.addWidget(self.editor.view, 1)
        split.addWidget(right)
        split.setStretchFactor(1, 3)
        layout.addWidget(split)
        tabs.addTab(lanes, "Lanes")
        self.controllers = ControllerEditor(session)
        tabs.addTab(self.controllers, "Controllers")
        self.setWidget(tabs)
        self._sources: list[LaneSource] = []
        session.changed.connect(self.refresh)
        selection.changed.connect(self.refresh)

    def _discrete(self, operation: Callable[..., None], *args: object) -> None:
        self.session.seal()
        operation(*args)

    # --- reading --------------------------------------------------------------------

    def refresh(self) -> None:
        """Re-read every lane and show the chosen one."""
        project = self.session.project
        if project is None:
            return
        chosen = self.lanes.currentItem().text() if self.lanes.currentItem() else ""
        sources = [
            self._clip_source(int(c["id"]), str(c["target"]), int(c["pattern"]))
            for c in project.automation_clips()
        ]
        sources += self._envelope_sources()
        self._sources = sources
        labels = [s.label for s in sources]
        if [self.lanes.item(i).text() for i in range(self.lanes.count())] != labels:
            self.lanes.blockSignals(True)
            self.lanes.clear()
            self.lanes.addItems(labels)
            self.lanes.blockSignals(False)
        if chosen in labels:
            self.lanes.setCurrentRow(labels.index(chosen))
        elif labels and self.lanes.currentRow() < 0:
            self.lanes.setCurrentRow(0)
        self._lane_chosen()
        self.controllers.refresh()

    def _lane_chosen(self) -> None:
        row = self.lanes.currentRow()
        self.editor.show(self._sources[row] if 0 <= row < len(self._sources) else None)

    def show_clip(self, clip: int) -> None:
        """Select the lane of automation clip ``clip``."""
        self.refresh()
        for index, source in enumerate(self._sources):
            if source.label.startswith(f"#{clip} "):
                self.lanes.setCurrentRow(index)
                return

    def _range(self, target: str) -> tuple[float, float, bool]:
        descriptor = self.session.require().param_descriptor(target)
        if descriptor is None:
            return (0.0, 1.0, False)
        low, high = float(descriptor["min"]), float(descriptor["max"])
        return (low, high, str(descriptor["scale"]) == "log" and low > 0)

    def _clip_source(self, clip: int, target: str, pattern: int) -> LaneSource:
        where = "playlist" if pattern == 0 else f"pattern {pattern}"
        project = self.session.require()

        def write(points: Points) -> None:
            self.session.build_and_execute("SetBreakpoints", clip, points)

        return LaneSource(
            label=f"#{clip} {target}  ({where})",
            read=lambda: project.automation_points(clip),
            write=write,
            lane=self._range(target),
        )

    def _envelope_sources(self) -> list[LaneSource]:
        """The selected placement's clip envelopes."""
        ids = self.selection.ids(Kind.CLIPS)
        if len(ids) != 1:
            return []
        project = self.session.require()
        items = project.playlist_items()
        rows = items[items["item"] == ids[0]]
        if not len(rows):
            return []
        track, item = int(rows[0]["track"]), int(rows[0]["item"])
        envelopes = project.item_envelopes(track, item)
        targets = [str(e["target"]) for e in envelopes] or ["gain"]
        out = []
        for index, target in enumerate(targets):
            out.append(
                LaneSource(
                    label=f"clip envelope: {target}  (item {item})",
                    read=self._envelope_reader(track, item, index),
                    write=self._envelope_writer(track, item, index, target),
                    lane=self._envelope_range(target),
                )
            )
        return out

    def _envelope_range(self, target: str) -> tuple[float, float, bool]:
        return {
            "gain": (0.0, 2.0, False),
            "pan": (-1.0, 1.0, False),
            "pitch": (-1200.0, 1200.0, False),
        }.get(target) or self._range(target)

    def _envelope_reader(self, track: int, item: int, index: int) -> Callable[[], Points]:
        def read() -> Points:
            envelopes = self.session.require().item_envelopes(track, item)
            if index < len(envelopes):
                return np.asarray(envelopes[index]["points"], dtype=np.float64)
            return np.zeros((0, 8), dtype=np.float64)

        return read

    def _envelope_writer(
        self, track: int, item: int, index: int, target: str
    ) -> Callable[[Points], None]:
        def write(points: Points) -> None:
            envelopes = self.session.require().item_envelopes(track, item)
            out = [(str(e["target"]), np.asarray(e["points"], dtype=np.float64)) for e in envelopes]
            if index < len(out):
                out[index] = (target, points)
            else:
                out.append((target, points))
            self.session.build_and_execute("SetClipEnvelopes", track, item, out)

        return write

    def apply_theme(self, theme: Theme) -> None:
        """Recolour."""
        self.editor.view.apply_theme(theme)
