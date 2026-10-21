"""The playlist: the arrangement (phase_6.md 4.1). The second hot surface.

Same pipeline as the piano roll: PlaylistGeometry builds every visible clip in C++, the
GeometryItem draws it, and a pan or zoom inside the built range is a transform - no
engine call. Track headers, the marker ruler and clip names are light widgets around it.
Placements are never kept here: every gesture re-reads them, every edit is a command, and
after every change the session tells this panel to rebuild from the engine.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PySide6.QtCore import QEvent, QObject, Qt, QTimer, Signal
from PySide6.QtGui import QAction, QActionGroup, QDragEnterEvent, QDropEvent
from PySide6.QtWidgets import QHBoxLayout, QVBoxLayout, QWidget

from adx import dnd, engine_bridge
from adx.engine_bridge import PPQ, AutomationGeometry, PeakCache, PlaylistGeometry
from adx.panels.base import DockPanel
from adx.panels.geometry_view import (
    CTRL,
    SHIFT,
    WORLD_LINES,
    WORLD_TRIANGLES,
    GeometryView,
    Pointer,
)
from adx.panels.playlist.toolbar import PlaylistToolbar
from adx.panels.playlist.tools import TOOLS, Tool, band_items
from adx.panels.playlist.tracks import TrackHeaders
from adx.panels.playlist.view_state import Built, PlaylistView
from adx.selection import Context, Kind, Selection
from adx.session import Session
from adx.theme.tokens import DARK, Theme
from adx.widgets.ruler import Ruler
from adx.widgets.timeline import Timeline

#: Layers: lanes, grid, clips, previews, curves (PlaylistGeometry), then the selected
#: clip's envelope line and points (AutomationGeometry).
MODES = (
    WORLD_TRIANGLES,
    WORLD_LINES,
    WORLD_TRIANGLES,
    WORLD_TRIANGLES,
    WORLD_LINES,
    WORLD_LINES,
    WORLD_TRIANGLES,
)
#: Build every row: a vertical scroll never rebuilds.
ALL_ROWS = 1.0e6
MAX_LABELS = 400


class PlaylistPanel(DockPanel):
    """The arrangement: tracks of pattern, audio and automation clips."""

    panel_id = "playlist"
    title = "Playlist"
    default_area = Qt.DockWidgetArea.RightDockWidgetArea

    #: A pattern id: open it in the channel rack and piano roll.
    open_pattern = Signal(int)

    def __init__(
        self, session: Session, selection: Selection, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.selection = selection
        self.view = PlaylistView()
        self.builder = PlaylistGeometry()
        self.envelope = AutomationGeometry()
        self.peaks = PeakCache()
        self.built: Built | None = None
        self.tools: dict[str, Tool] = {cls.name: cls(self) for cls in TOOLS}
        self.tool: Tool = self.tools["draw"]
        self.follow = False
        self._pan: tuple[float, float, float, float] | None = None
        self._peak_timer = QTimer(self)
        self._peak_timer.setSingleShot(True)
        self._peak_timer.setInterval(120)
        self._peak_timer.timeout.connect(lambda: self.rebuild(force=True))
        self._build_ui()
        session.changed.connect(self.refresh)
        selection.changed.connect(lambda: self.rebuild(force=True))

    def _build_ui(self) -> None:
        body = QWidget()
        outer = QVBoxLayout(body)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)
        self.toolbar = PlaylistToolbar(self)
        outer.addWidget(self.toolbar)
        top = QHBoxLayout()
        top.setSpacing(0)
        self.ruler = Ruler(PPQ, markers=True)
        self.ruler.snap_ticks = self.toolbar.snap_ticks
        self.ruler.seek_requested.connect(lambda t: self.session.engine.seek(int(t)))
        self.ruler.marker_added.connect(self._add_marker)
        self.ruler.marker_moved.connect(
            lambda m, t, n: self.session.build_and_execute("MoveMarker", m, t, n)
        )
        self.ruler.marker_removed.connect(
            lambda m: self.session.build_and_execute("RemoveMarker", m)
        )
        self.headers = TrackHeaders()
        self.headers.changed.connect(self._track_changed)
        spacer = QWidget()
        spacer.setFixedWidth(self.headers.width())
        top.addWidget(spacer)
        top.addWidget(self.ruler, 1)
        outer.addLayout(top)
        middle = QHBoxLayout()
        middle.setSpacing(0)
        middle.addWidget(self.headers)
        self.canvas = GeometryView(self, MODES)
        self.canvas.setAcceptDrops(True)
        self.canvas.installEventFilter(self)
        middle.addWidget(self.canvas, 1)
        outer.addLayout(middle, 1)
        self.timeline = Timeline()
        self.timeline.scroll_requested.connect(self._scroll_to)
        outer.addWidget(self.timeline)
        self.setWidget(body)

    # --- reading --------------------------------------------------------------------

    def refresh(self) -> None:
        """After any change: re-read and rebuild."""
        project = self.session.project
        if project is None:
            return
        self.toolbar.refresh_choices(project)
        self.ruler.set_markers(project.markers())
        self.ruler.set_meter(*project.meter_at(0))
        self.timeline.set_length(max(project.content_length(), PPQ * 64))
        existing = {int(i) for i in project.playlist_items()["item"]}
        if self.selection.kind is Kind.CLIPS:
            self.selection.prune(existing)
        self.rebuild(force=True)

    def rebuild(self, force: bool = False) -> None:
        """Build the visible range and upload: only when needed, unless forced."""
        project = self.session.project
        if project is None:
            return
        if not force and not self.view.needs_rebuild(self.built):
            return
        built = self.view.build_range()
        selected = np.asarray(self.selection.ids(Kind.CLIPS), dtype=np.uint32)
        self.builder.build(
            project,
            (
                built.tick_start,
                built.tick_end,
                0.0,
                ALL_ROWS,
                built.pixels_per_tick,
                built.pixels_per_row,
            ),
            selected,
            self.peaks,
        )
        for layer in range(5):
            with self.builder.view(layer) as lease:
                self.canvas.upload(layer, lease)
        self.built = built
        self._rebuild_envelope()
        pending, _detailed = self.builder.status()
        if pending:
            self._peak_timer.start()
        self.rebuild_headers()
        self.rebuild_labels()

    def rebuild_headers(self) -> None:
        """Track headers from a fresh read and the builder's layout."""
        project = self.session.require()
        tracks = list(project.playlist_info()["tracks"])
        rows = [(int(r[0]), float(r[1]), float(r[2])) for r in self.builder.rows()]
        self.headers.show_tracks(tracks, rows, self.view.row_start, self.view.pixels_per_row)

    def rebuild_labels(self) -> None:
        """Names over every clip wide enough to read one."""
        project = self.session.require()
        visible = self.builder.visible()
        if len(visible) == 0:
            self.canvas.set_labels([])
            return
        patterns = {int(p["id"]): str(p["name"]) for p in project.patterns_info()}
        samples = {i: Path(path).name for i, path, _full in project.samples()}
        lanes = {int(c["id"]): str(c["target"]) for c in project.automation_clips()}
        names = (patterns, samples, lanes)
        labels: list[list[object]] = []
        v = self.view
        for row in visible:
            width = (row[3] - row[2]) * v.pixels_per_tick
            x = v.x_of(row[2])
            if width < 30 or x + width < 0 or x > v.width:
                continue
            text = names[int(row[6])].get(int(row[7]), "?")
            labels.append([max(0.0, x), v.y_of(row[4]), width, text])
            if len(labels) >= MAX_LABELS:
                break
        self.canvas.set_labels(labels)

    def _rebuild_envelope(self) -> None:
        """The selected clip's first envelope, drawn on the clip (§4.5)."""
        project = self.session.require()
        ids = self.selection.ids(Kind.CLIPS)
        row = self.item_row(ids[0]) if len(ids) == 1 else None
        envelopes = (
            project.item_envelopes(int(row["track"]), int(row["item"])) if row is not None else []
        )
        if row is None or not envelopes:
            for layer in (5, 6):
                self.canvas.upload_raw(layer, 0, 0, 0)
            return
        envelope = envelopes[0]
        lane = self.envelope_range(str(envelope["target"]))
        rows = {int(r[0]): (float(r[1]), float(r[2])) for r in self.builder.rows()}
        top, height = rows.get(int(row["track"]), (0.0, 1.0))
        built = self.built or self.view.build_range()
        self.envelope.build(
            np.asarray(envelope["points"], dtype=np.float64),
            lane,
            (
                built.tick_start,
                built.tick_end,
                built.pixels_per_tick,
                height * built.pixels_per_row,
            ),
            np.zeros(0, dtype=np.uint8),
            place=(float(row["start"]), top + height * 0.25, height * 0.7),
        )
        for layer, buffer in ((5, 0), (6, 1)):
            with self.envelope.view(buffer) as lease:
                self.canvas.upload(layer, lease)

    def envelope_range(self, target: str) -> tuple[float, float, bool]:
        """A clip envelope's value range: gain 0..2, pan -1..1, pitch +-1200 cents, or the
        target parameter's own."""
        if target == "gain":
            return (0.0, 2.0, False)
        if target == "pan":
            return (-1.0, 1.0, False)
        if target == "pitch":
            return (-1200.0, 1200.0, False)
        descriptor = self.session.require().param_descriptor(target)
        if descriptor is None:
            return (0.0, 1.0, False)
        return (
            float(descriptor["min"]),
            float(descriptor["max"]),
            str(descriptor["scale"]) == "log" and float(descriptor["min"]) > 0,
        )

    # --- what the tools ask ---------------------------------------------------------

    def hit(self, x: float, y: float) -> tuple[int, int, int]:
        """(track, item or 0, part) under a pixel: the engine's hit test."""
        return self.builder.hit_test(self.view.hit_view(), x, y)

    def track_at(self, y: float) -> int:
        """The track under a pixel row, or 0."""
        return self.hit(-1.0e9, y)[0]

    def track_rows(self) -> list[tuple[int, float, float]]:
        """(track, top row, height) per track, from the last build."""
        return [(int(r[0]), float(r[1]), float(r[2])) for r in self.builder.rows()]

    def item_row(self, item: int) -> np.void | None:
        """One placement, read now."""
        items = self.session.require().playlist_items()
        rows = items[items["item"] == item]
        return rows[0] if len(rows) else None

    def item_kind(self, item: int) -> int:
        """0 pattern, 1 audio, 2 automation; -1 when gone."""
        row = self.item_row(item)
        return -1 if row is None else int(row["kind"])

    def snap_ticks(self) -> int:
        """The snap grid in ticks, 0 for none."""
        return self.toolbar.snap_ticks()

    def snap_tick(self, tick: float) -> int:
        """Down to the grid."""
        grid = self.snap_ticks()
        return max(0, int(tick // grid) * grid) if grid > 0 else max(0, round(tick))

    def snap_delta(self, delta: float) -> int:
        """A drag distance, to the nearest grid step."""
        grid = self.snap_ticks()
        return round(delta / grid) * grid if grid > 0 else round(delta)

    def min_length(self) -> int:
        """The shortest a clip can be trimmed to."""
        return max(self.snap_ticks(), PPQ // 16)

    def pattern_length(self) -> int:
        """The draw pattern's length."""
        pattern = self.toolbar.pattern_id()
        info = next(
            (p for p in self.session.require().patterns_info() if int(p["id"]) == pattern), None
        )
        return int(info["length"]) if info is not None else 0

    def edit_item(self, track: int, item: int, fields: dict[str, float]) -> None:
        """One SetPlaylistItem: consecutive ones on an item coalesce into one step."""
        self.session.build_and_execute("SetPlaylistItem", track, item, fields)

    def place_pattern(self, pointer: Pointer) -> None:
        """The draw pattern at a click."""
        track = self.track_at(pointer.y)
        if track:
            self.add_pattern_at(track, self.snap_tick(self.view.tick_at(pointer.x)))

    def add_pattern_at(self, track: int, tick: int) -> None:
        """The draw pattern on ``track`` at ``tick``."""
        pattern = self.toolbar.pattern_id()
        if pattern:
            self.session.build_and_execute("AddPatternItem", track, pattern, tick, 0)

    def slice(self, track: int, item: int, at: int) -> None:
        """Cut a clip in two at ``at``: the original shortened, the rest a new clip."""
        row = self.item_row(item)
        if row is None:
            return
        start, length, offset = int(row["start"]), int(row["length"]), int(row["offset"])
        if not start < at < start + length:
            return
        project = self.session.require()
        with self.session.group("Slice clip"):
            self.edit_item(track, item, {"length": at - start})
            if int(row["kind"]) == 0:
                self.session.build_and_execute(
                    "AddPatternItem", track, int(row["ref"]), at, start + length - at
                )
                new = int(project.playlist_items()["item"].max())
                self.edit_item(track, new, {"offset": offset + (at - start)})
            elif int(row["kind"]) == 1:
                samples = {i: full for i, _p, full in project.samples()}
                new = project.add_audio_item(
                    track, samples[int(row["ref"])], at, start + length - at
                )
                self.edit_item(
                    track, new, {"offset": offset + (at - start), "stretch": float(row["stretch"])}
                )

    def group_tracks(self, parent: int, members: list[int]) -> None:
        """Members join ``parent``'s group: one step."""
        with self.session.group("Group tracks"):
            for member in members:
                self.session.build_and_execute("SetPlaylistTrack", member, {"group": parent})

    def show_band(self, band: tuple[float, float, float, float] | None) -> None:
        """The rubber band."""
        self.canvas.set_selection_rect(band)

    def select_band(self, band: tuple[float, float, float, float], add: bool) -> None:
        """Select every clip the band touches."""
        chosen = band_items(self.builder.visible(), band, self.view.hit_view())
        if add:
            chosen = sorted(set(chosen) | set(self.selection.ids(Kind.CLIPS)))
        self.selection.select(Kind.CLIPS, chosen, Context())

    def set_tool(self, name: str) -> None:
        """Choose a tool."""
        self.tool = self.tools[name]
        self.canvas.set_hint(self.tool.label)
        if name in self._tool_actions:
            self._tool_actions[name].setChecked(True)

    # --- input ----------------------------------------------------------------------

    def on_press(self, pointer: Pointer) -> None:
        if pointer.button == int(Qt.MouseButton.MiddleButton.value):
            self._pan = (pointer.x, pointer.y, self.view.tick_start, self.view.row_start)
            return
        if pointer.button == int(Qt.MouseButton.RightButton.value):
            track, item, _part = self.hit(pointer.x, pointer.y)
            if item:
                self.session.build_and_execute("RemovePlaylistItem", track, item)
            return
        self.tool.press(pointer)

    def on_move(self, pointer: Pointer) -> None:
        if self._pan is not None:
            x0, y0, tick0, row0 = self._pan
            self.view.tick_start = max(0.0, tick0 - (pointer.x - x0) / self.view.pixels_per_tick)
            self.view.row_start = max(0.0, row0 - (pointer.y - y0) / self.view.pixels_per_row)
            self.apply_view()
            return
        self.tool.move(pointer)

    def on_release(self, pointer: Pointer) -> None:
        if self._pan is not None:
            self._pan = None
            return
        self.tool.release(pointer)

    def on_double_click(self, pointer: Pointer) -> None:
        _track, item, _part = self.hit(pointer.x, pointer.y)
        row = self.item_row(item) if item else None
        if row is not None and int(row["kind"]) == 0:
            self.open_pattern.emit(int(row["ref"]))

    def on_wheel(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None:
        if modifiers & CTRL:
            self.view.zoom(1.15 if dy > 0 else 1 / 1.15, x)
        elif modifiers & SHIFT:
            self.view.tick_start = max(
                0.0, self.view.tick_start - dy / self.view.pixels_per_tick * 0.4
            )
        else:
            self.view.row_start = max(0.0, self.view.row_start - dy / 120.0)
        self.apply_view()

    def on_resize(self, width: float, height: float) -> None:
        self.view.width, self.view.height = max(1.0, width), max(1.0, height)
        self.apply_view()

    def apply_view(self) -> None:
        """Move the transform; rebuild only if the view left the built range."""
        self.canvas.set_view(*self.view.transform())
        self.ruler.set_view(self.view.tick_start, self.view.pixels_per_tick)
        self.timeline.set_window(self.view.tick_start, self.view.tick_end)
        self.headers.place(self.view.row_start, self.view.pixels_per_row)
        self.rebuild()
        if self.session.project is not None:
            self.rebuild_labels()

    def _scroll_to(self, tick: float) -> None:
        self.view.tick_start = max(0.0, tick)
        self.apply_view()

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802 - Qt's name
        if watched is self.canvas and event.type() in (QEvent.Type.DragEnter, QEvent.Type.DragMove):
            assert isinstance(event, QDragEnterEvent)
            payload = dnd.decode(event.mimeData())
            if payload is not None and isinstance(payload, dnd.SampleFile | dnd.PatternRef):
                event.acceptProposedAction()
                return True
        if watched is self.canvas and event.type() == QEvent.Type.Drop:
            assert isinstance(event, QDropEvent)
            payload = dnd.decode(event.mimeData())
            position = event.position()
            track = self.track_at(position.y())
            if payload is not None and track:
                tick = self.snap_tick(self.view.tick_at(position.x()))
                dnd.perform(self.session, payload, dnd.PlaylistTarget(track, tick))
                event.acceptProposedAction()
                return True
        return super().eventFilter(watched, event)

    # --- the rest -------------------------------------------------------------------

    def _track_changed(self, track: int, fields: dict[str, object]) -> None:
        self.session.build_and_execute("SetPlaylistTrack", track, fields)

    def _add_marker(self, tick: int) -> None:
        count = len(self.session.require().markers())
        self.session.execute(engine_bridge.commands.add_marker(tick, f"Marker {count + 1}"))

    def frame(self, position_ticks: int, rolling: bool) -> None:
        """The 60 Hz frame: the playhead moves; nothing is rebuilt."""
        self.canvas.set_playhead(position_ticks / PPQ)
        self.ruler.set_playhead(position_ticks)
        self.timeline.set_playhead(position_ticks)
        if (
            rolling
            and self.follow
            and not self.view.tick_start <= position_ticks < self.view.tick_end
        ):
            self.view.tick_start = float(position_ticks)
            self.apply_view()

    def apply_theme(self, theme: Theme = DARK) -> None:
        """Recolour: no rebuild."""
        self.canvas.apply_theme(theme)
        self.ruler.set_theme(theme)
        self.timeline.set_theme(theme)

    def tool_actions(self) -> list[QAction]:
        """The tool buttons (for the toolbar)."""
        group = QActionGroup(self)
        actions = []
        self._tool_actions: dict[str, QAction] = {}
        for tool in self.tools.values():
            action = QAction(tool.label, self, checkable=True)
            action.setToolTip(f"{tool.label} ({tool.shortcut})")
            action.setChecked(tool is self.tool)
            action.triggered.connect(lambda _c=False, n=tool.name: self.set_tool(n))
            group.addAction(action)
            actions.append(action)
            self._tool_actions[tool.name] = action
        return actions
