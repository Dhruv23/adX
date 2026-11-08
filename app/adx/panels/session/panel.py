"""The session view (phase_11.md 4.9): scenes down, tracks across, a launchable clip in
each cell. Click a cell to launch it (an empty one stops its track), a track's square
to stop it, a scene's arrow to fire the row. Right-click a cell to put a pattern in it,
clear it or set how it launches; the toolbar adds tracks and scenes and sets the global
quantize.

The grid is the same geometry pipeline as every other hot surface: SessionGeometry
builds every cell, the playing cells' progress rings and the queued cells' marks from
the clip states in one C++ call, and GeometryView draws them - one build and two leases
a frame however many cells there are. Names are labels over the cells. The track heads,
stop squares and scene buttons around the grid are ordinary widgets.

Every edit is one SetSession command through the session, so it is undoable and in the
text as [SESSION].
"""

from __future__ import annotations

import copy
from functools import partial
from typing import Any

import numpy as np
from PySide6.QtCore import QPoint, Qt, QTimer
from PySide6.QtWidgets import (
    QComboBox,
    QGridLayout,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QMenu,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from adx.engine_bridge import SessionGeometry
from adx.panels.base import DockPanel
from adx.panels.geometry_view import LEFT, RIGHT, WORLD_TRIANGLES, GeometryView, Pointer
from adx.session import Session

QUANTIZES = ("instant", "1/4", "1/2", "bar", "2bar", "4bar", "8bar")
FOLLOWS = ("none", "stop", "next", "previous", "first", "random", "repeat")
#: A cell's size in pixels.
CELL_W = 96
CELL_H = 28


class SessionGrid:
    """The cells: geometry in, clicks out."""

    def __init__(self, panel: SessionPanel) -> None:
        self.panel = panel
        self.builder = SessionGeometry()
        self.view = GeometryView(self, (WORLD_TRIANGLES, WORLD_TRIANGLES))
        self.view.setObjectName("session-grid")
        self.tracks = 0
        self.rows = 0
        self.filled = np.zeros(0, dtype=np.uint8)
        self.builds = 0
        self.last: tuple[int, int] = (0, 0)

    def layout(self, tracks: int, rows: int, cells: dict[tuple[int, int], Any]) -> None:
        self.tracks, self.rows = tracks, rows
        self.filled = np.zeros(max(tracks * rows, 1), dtype=np.uint8)
        labels: list[list[object]] = []
        for (t, r), cell in cells.items():
            if cell is not None:
                self.filled[t * rows + r] = 1
                labels.append([t * CELL_W + 6, r * CELL_H + 6, CELL_W - 30, str(cell["pattern"])])
        self.view.setFixedSize(max(tracks * CELL_W, 1), max(rows * CELL_H, 1))
        self.view.set_view(0.0, 0.0, float(CELL_W), float(CELL_H))
        self.view.set_labels(labels)
        self.frame()

    def frame(self) -> None:
        """One build from the clip states, and one upload per layer."""
        if self.tracks == 0 or self.rows == 0:
            self.view.clear()
            return
        engine = self.panel.session.engine
        recording = engine.performance_recording
        self.last = self.builder.build(engine, self.tracks, self.rows, self.filled, recording)
        for layer in (0, 1):
            with self.builder.view(layer) as lease:
                self.view.upload(layer, lease)
        self.builds += 1

    def cell_at(self, x: float, y: float) -> tuple[int, int] | None:
        t, r = int(x // CELL_W), int(y // CELL_H)
        if 0 <= t < self.tracks and 0 <= r < self.rows:
            return t, r
        return None

    # --- input ----------------------------------------------------------------------

    def on_press(self, pointer: Pointer) -> None:
        cell = self.cell_at(pointer.x, pointer.y)
        if cell is None:
            return
        if pointer.button == LEFT:
            self.panel.launch(*cell)
        elif pointer.button == RIGHT:
            at = self.view.mapToGlobal(QPoint(int(pointer.x), int(pointer.y)))
            self.panel.cell_menu(cell[0], cell[1], at)

    def on_move(self, pointer: Pointer) -> None:
        pass

    def on_release(self, pointer: Pointer) -> None:
        pass

    def on_double_click(self, pointer: Pointer) -> None:
        pass

    def on_wheel(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None:
        pass

    def on_resize(self, width: float, height: float) -> None:
        pass


class SessionPanel(DockPanel):
    """The clip grid."""

    panel_id = "session"
    title = "Session"
    default_area = Qt.DockWidgetArea.BottomDockWidgetArea
    #: A grid grows with its scenes; scrolling keeps it from pinning the splitter beside it.
    scrolls = True

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.session = session
        #: Each cell's clip (session_info's slot dict) or None, by (track, row).
        self.cells: dict[tuple[int, int], dict[str, Any] | None] = {}
        body = QWidget()
        outer = QVBoxLayout(body)
        bar = QHBoxLayout()
        self.add_track = QPushButton("+ Track")
        self.add_track.setObjectName("session-add-track")
        self.add_track.clicked.connect(self._add_track)
        self.add_scene = QPushButton("+ Scene")
        self.add_scene.setObjectName("session-add-scene")
        self.add_scene.clicked.connect(self._add_scene)
        self.quantize = QComboBox()
        self.quantize.setObjectName("session-quantize")
        self.quantize.addItems([q for q in QUANTIZES if q != "instant"])
        self.quantize.setToolTip("Where a launch lands unless the clip says otherwise")
        self.quantize.activated.connect(self._quantize_chosen)
        self.stop_all = QPushButton("■ Stop all")
        self.stop_all.setObjectName("session-stop-all")
        self.stop_all.clicked.connect(lambda: self.session.engine.stop_all_clips())
        self.pads_button = QPushButton("Pads…")
        self.pads_button.setObjectName("session-pads")
        self.pads_button.setToolTip("Pad controllers: which input, which profile, learn mode")
        self.pads_button.clicked.connect(self.pad_setup)
        #: The main window's pad controllers, when there is one (set by it).
        self.controllers: Any = None
        for widget in (
            self.add_track,
            self.add_scene,
            QLabel(" Quantize"),
            self.quantize,
            self.stop_all,
            self.pads_button,
        ):
            bar.addWidget(widget)
        bar.addStretch(1)
        outer.addLayout(bar)
        self.frame_area = QGridLayout()
        self.frame_area.setSpacing(0)
        self.frame_area.setContentsMargins(0, 0, 0, 0)
        self.heads = QHBoxLayout()
        self.heads.setSpacing(0)
        self.stops = QHBoxLayout()
        self.stops.setSpacing(0)
        self.scenes = QVBoxLayout()
        self.scenes.setSpacing(0)
        self.grid = SessionGrid(self)
        self.frame_area.addLayout(self.heads, 0, 0, Qt.AlignmentFlag.AlignLeft)
        self.frame_area.addWidget(self.grid.view, 1, 0, Qt.AlignmentFlag.AlignTop)
        self.frame_area.addLayout(self.scenes, 1, 1, Qt.AlignmentFlag.AlignTop)
        self.frame_area.addLayout(self.stops, 2, 0, Qt.AlignmentFlag.AlignLeft)
        outer.addLayout(self.frame_area)
        outer.addStretch(1)
        self.setWidget(body)
        self.timer = QTimer(self)
        self.timer.setInterval(33)
        self.timer.timeout.connect(self.refresh_states)
        self.timer.start()
        session.changed.connect(self.rebuild)
        self.rebuild()

    # --- the model ------------------------------------------------------------------

    def info(self) -> dict[str, Any]:
        project = self.session.project
        if project is None:
            return {"quantize": "bar", "scenes": [], "tracks": []}
        return dict(project.session_info())

    def _apply(self, info: dict[str, Any]) -> None:
        project = self.session.project
        if project is not None:
            self.session.call(project.set_session, info)

    def _add_track(self) -> None:
        project = self.session.project
        if project is None:
            return
        info = self.info()
        used = {t["channel"] for t in info["tracks"]}
        names = [str(c["name"]) for c in project.channels_info() if str(c["name"]) not in used]
        if not names:
            self.session.failed.emit("Session", "every channel already has a track")
            return
        name, ok = QInputDialog.getItem(self, "Add track", "Channel", names, 0, False)
        if ok:
            info["tracks"].append({"channel": name, "slots": [None] * len(info["scenes"])})
            self._apply(info)

    def _add_scene(self) -> None:
        info = self.info()
        info["scenes"].append({"name": f"Scene {len(info['scenes']) + 1}", "tempo": None})
        for track in info["tracks"]:
            track["slots"].append(None)
        self._apply(info)

    def _quantize_chosen(self, _index: int) -> None:
        info = self.info()
        info["quantize"] = self.quantize.currentText()
        self._apply(info)

    def set_cell(self, track: int, row: int, cell: dict[str, Any] | None) -> None:
        """Put a clip (or nothing) in a cell, as one undoable edit."""
        info = copy.deepcopy(self.info())
        slots = info["tracks"][track]["slots"]
        while len(slots) <= row:
            slots.append(None)
        slots[row] = cell
        self._apply(info)

    def set_scene_tempo(self, row: int, bpm: float | None) -> None:
        """A scene's tempo, taken over when it fires; None keeps the tempo."""
        info = copy.deepcopy(self.info())
        info["scenes"][row]["tempo"] = bpm
        self._apply(info)

    # --- the grid -------------------------------------------------------------------

    @staticmethod
    def _clear(layout: QHBoxLayout | QVBoxLayout) -> None:
        while layout.count():
            item = layout.takeAt(0)
            widget = item.widget() if item is not None else None
            if widget is not None:
                widget.deleteLater()

    def cell_text(self, track: int, row: int) -> str:
        cell = self.cells.get((track, row))
        return str(cell["pattern"]) if cell else ""

    def rebuild(self) -> None:
        """Re-lay the grid from the model."""
        for layout in (self.heads, self.stops, self.scenes):
            self._clear(layout)
        info = self.info()
        index = self.quantize.findText(str(info["quantize"]))
        if index >= 0:
            self.quantize.setCurrentIndex(index)
        rows = len(info["scenes"])
        self.cells = {}
        for t, track in enumerate(info["tracks"]):
            head = QLabel(str(track["channel"]))
            head.setAlignment(Qt.AlignmentFlag.AlignCenter)
            head.setFixedWidth(CELL_W)
            self.heads.addWidget(head)
            for r in range(rows):
                self.cells[(t, r)] = track["slots"][r] if r < len(track["slots"]) else None
            stop = QPushButton("■")
            stop.setObjectName(f"session-stop:{t}")
            stop.setFixedWidth(CELL_W)
            stop.setToolTip("Stop this track's clip")
            stop.clicked.connect(lambda _c=False, t=t: self.session.engine.stop_clip(t))
            self.stops.addWidget(stop)
        for r, scene in enumerate(info["scenes"]):
            tempo = f"  {scene['tempo']:g} bpm" if scene.get("tempo") else ""
            fire = QPushButton(f"▶ {scene['name']}{tempo}")
            fire.setObjectName(f"session-scene:{r}")
            fire.setFixedHeight(CELL_H)
            fire.setToolTip("Fire this scene: every track's clip in the row")
            fire.clicked.connect(lambda _c=False, r=r: self.session.engine.fire_scene(r))
            self.scenes.addWidget(fire)
        self.grid.layout(len(info["tracks"]), rows, self.cells)

    def pad_setup(self) -> None:
        from adx.panels.session.pad_setup import PadSetupDialog

        if self.controllers is None:
            self.session.failed.emit("Pads", "no MIDI here (a null backend)")
            return
        PadSetupDialog(self.controllers, self.controllers.settings_dir, self).exec()

    def launch(self, track: int, row: int) -> None:
        """A cell's click: launch its clip, or stop the track if the cell is empty."""
        if self.cells.get((track, row)) is not None:
            self.session.engine.launch_clip(track, row)
        else:
            self.session.engine.stop_clip(track)

    def refresh_states(self) -> None:
        """The grid from the clip states: one build, at 30 Hz."""
        self.grid.frame()

    def state_of(self, track: int, row: int) -> str:
        return self.session.engine.clip_state(track, row)

    def cell_menu(self, track: int, row: int, at: QPoint) -> QMenu | None:
        project = self.session.project
        if project is None:
            return None
        cell = self.cells.get((track, row))
        menu = QMenu(self)
        put = menu.addMenu("Put pattern")
        for pattern in project.patterns_info():
            name = str(pattern["name"])
            put.addAction(name, partial(self.set_cell, track, row, {"pattern": name}))
        if cell is not None:
            quantize = menu.addMenu("Launch quantize")
            for q in ("global", *QUANTIZES):
                quantize.addAction(q, partial(self.set_cell, track, row, {**cell, "quantize": q}))
            follow = menu.addMenu("Follow action")
            for f in FOLLOWS:
                follow.addAction(f, partial(self.set_cell, track, row, {**cell, "follow": f}))
            loop = {**cell, "loop": not cell["loop"]}
            menu.addAction(
                "Play once" if cell["loop"] else "Loop", partial(self.set_cell, track, row, loop)
            )
            legato = {**cell, "legato": not cell["legato"]}
            menu.addAction(
                "Legato off" if cell["legato"] else "Legato",
                partial(self.set_cell, track, row, legato),
            )
            menu.addAction("Clear", partial(self.set_cell, track, row, None))
        menu.popup(at)
        return menu
