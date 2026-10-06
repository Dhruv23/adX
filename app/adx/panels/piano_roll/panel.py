"""The piano roll panel: the dock, its toolbar, and what a click means (phase_5.md 4.2).

Python owns the layout, the viewport and the tool state; C++ owns the geometry and the
drawing. The load-bearing rules, as this file keeps them:

- **a pan or zoom rebuilds nothing** - it moves the scene-graph item's transform
  (:meth:`PianoRollPanel.apply_view`) and makes no engine call unless the view has left
  the built range or crossed the level of detail;
- **a selection change rebuilds nothing** - it recolours vertices in place;
- **the playhead never dirties the notes** - it is its own node, moved once a frame
  from the frame clock's single engine call;
- **every gesture is one command** - the tools end in ``RollModel.commit``.
"""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

import numpy as np
from PySide6.QtCore import QEvent, QObject, QSettings, Qt, QUrl, Slot
from PySide6.QtGui import QAction, QActionGroup, QColor, QKeyEvent, QKeySequence
from PySide6.QtQuick import QQuickItem
from PySide6.QtQuickWidgets import QQuickWidget
from PySide6.QtWidgets import (
    QComboBox,
    QGridLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from adx import engine_bridge
from adx.engine_bridge import PPQ, NoteArray, PianoRollGeometry, Project
from adx.panels.base import DockPanel
from adx.panels.piano_roll import chords, quantize, scales
from adx.panels.piano_roll.model import LANES, RollModel
from adx.panels.piano_roll.tools import (
    ALT,
    CTRL,
    KEY_DELETE,
    LEFT,
    MIDDLE,
    SHIFT,
    TOOLS,
    LaneEditor,
    Pointer,
    SelectTool,
    Tool,
    simplify_curve,
)
from adx.panels.piano_roll.view_state import BuiltRange, ViewState
from adx.quick import ULong, call
from adx.theme.tokens import DARK, Theme, qml_palette, role_colors
from adx.widgets.keyboard import PianoKeyboard
from adx.widgets.ruler import Ruler
from adx.widgets.timeline import Timeline

VIEW_QML = Path(__file__).with_name("view.qml")
#: The lane index the lyric cells use (after the engine's six numeric lanes).
LYRIC_LANE = len(LANES)


class RollInput(QObject):
    """Where view.qml forwards the C++ item's raw input."""

    def __init__(self, panel: PianoRollPanel) -> None:
        super().__init__(panel)
        self._panel = panel

    @Slot(float, float, int, int)
    def pressed(self, x: float, y: float, button: int, modifiers: int) -> None:
        self._panel.on_press(Pointer(x, y, button, modifiers))

    @Slot(float, float, int, int)
    def moved(self, x: float, y: float, buttons: int, modifiers: int) -> None:
        self._panel.on_move(Pointer(x, y, buttons, modifiers))

    @Slot(float, float, int, int)
    def released(self, x: float, y: float, button: int, modifiers: int) -> None:
        self._panel.on_release(Pointer(x, y, button, modifiers))

    @Slot(float, float, int)
    def doubleClicked(self, x: float, y: float, modifiers: int) -> None:  # noqa: N802 - QML
        self._panel.on_double_click(x, y)

    @Slot(float, float, int)
    def hovered(self, x: float, y: float, modifiers: int) -> None:
        self._panel.on_hover(x, y)

    @Slot(float, float, float, float, int)
    def wheeled(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None:
        self._panel.on_wheel(x, y, dx, dy, modifiers)

    @Slot(int, int, str)
    def keyPressed(self, key: int, modifiers: int, text: str) -> None:  # noqa: N802 - QML
        self._panel.on_key(key, modifiers)

    @Slot(float, float)
    def resized(self, width: float, height: float) -> None:
        self._panel.on_resize(width, height)


class PianoRollPanel(DockPanel):
    """One channel's notes in one pattern, edited with nine tools and six lanes."""

    panel_id = "piano_roll"
    title = "Piano roll"
    default_area = Qt.DockWidgetArea.RightDockWidgetArea

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.theme: Theme = DARK
        self.project: Project | None = None
        self.model: RollModel | None = None
        self.on_edit: Callable[[], None] | None = None
        self.view = ViewState()
        self.builder = PianoRollGeometry()
        self.built: BuiltRange | None = None
        self.tools: dict[str, Tool] = {cls().name: cls() for cls in TOOLS}
        self.tools["chord"] = chords.ChordTool()
        self.tool: Tool = self.tools["draw"]
        self.lane_editor = LaneEditor()
        self.ghosts = True
        self.follow = True
        self.humanize_seed = 1
        self._pan_origin: tuple[float, float] | None = None
        self._placements = np.zeros((0, 3), dtype=np.int64)
        self._lyric_ids = np.zeros(0, dtype=np.uint32)
        self._lyric_note = 0
        self._build_ui()

    # --- construction ---------------------------------------------------------------

    def _build_ui(self) -> None:
        body = QWidget()
        outer = QVBoxLayout(body)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)
        for bar in self._build_toolbars():
            outer.addWidget(bar)
        self.ruler = Ruler(PPQ)
        self.keyboard = PianoKeyboard()
        self.timeline = Timeline()
        self.quick = QQuickWidget()
        self.quick.setResizeMode(QQuickWidget.ResizeMode.SizeRootObjectToView)
        self.input = RollInput(self)
        context = self.quick.rootContext()
        context.setContextProperty("rollInput", self.input)
        context.setContextProperty("adxTheme", qml_palette(self.theme))
        self.quick.setSource(QUrl.fromLocalFile(str(VIEW_QML)))
        errors = self.quick.errors()
        if errors:
            raise RuntimeError("piano roll QML failed: " + "; ".join(e.toString() for e in errors))
        root = self.quick.rootObject()
        item = root.findChild(QQuickItem, "roll") if root is not None else None
        if root is None or item is None:
            raise RuntimeError("piano roll QML has no PianoRollItem")
        self.root: QQuickItem = root
        self.item: QQuickItem = item
        grid = QGridLayout()
        grid.setSpacing(0)
        corner = QLabel()
        corner.setFixedSize(self.keyboard.width(), self.ruler.height())
        grid.addWidget(corner, 0, 0)
        grid.addWidget(self.ruler, 0, 1)
        grid.addWidget(self.keyboard, 1, 0)
        grid.addWidget(self.quick, 1, 1)
        grid.addWidget(self.timeline, 2, 1)
        outer.addLayout(grid, 1)
        self.setWidget(body)
        self.ruler.seek_requested.connect(self._seek_from_ruler)
        self.timeline.scroll_requested.connect(self._scroll_to)
        self._lyric_edit = QLineEdit(self.quick)
        self._lyric_edit.hide()
        self._lyric_edit.returnPressed.connect(lambda: self._commit_lyric(advance=False))
        self._lyric_edit.installEventFilter(self)
        self.seek: Callable[[int], None] | None = None
        self.apply_theme(self.theme)

    def _build_toolbars(self) -> tuple[QToolBar, QToolBar]:
        """Row one: what is being edited, and with which tool. Row two: how."""
        bar = QToolBar()
        first = bar
        self.pattern_box = QComboBox()
        self.pattern_box.setToolTip("Pattern")
        self.channel_box = QComboBox()
        self.channel_box.setToolTip("Channel")
        self.pattern_box.activated.connect(self._clip_chosen)
        self.channel_box.activated.connect(self._clip_chosen)
        bar.addWidget(self.pattern_box)
        bar.addWidget(self.channel_box)
        bar.addSeparator()
        group = QActionGroup(self)
        self.tool_actions: dict[str, QAction] = {}
        for index, (name, tool) in enumerate(self.tools.items()):
            action = QAction(tool.label, self, checkable=True)
            action.setShortcut(QKeySequence(str(index + 1)) if index < 9 else QKeySequence())
            action.setToolTip(f"{tool.label} ({index + 1})" if index < 9 else tool.label)
            action.triggered.connect(lambda _checked=False, n=name: self.set_tool(n))
            group.addAction(action)
            bar.addAction(action)
            self.tool_actions[name] = action
        self.tool_actions["draw"].setChecked(True)
        bar = QToolBar()
        self.grid_box = QComboBox()
        for label, _ticks in quantize.GRIDS:
            self.grid_box.addItem(label)
        self.grid_box.addItem("Off")
        self.grid_box.setCurrentIndex(quantize.grid_index(PPQ // 4))
        self.grid_box.setToolTip("Snap grid")
        self.grid_box.currentIndexChanged.connect(self._grid_chosen)
        bar.addWidget(self.grid_box)
        self.lane_box = QComboBox()
        self.lane_box.addItems([*LANES, "Lyric"])
        self.lane_box.setToolTip("Lane")
        self.lane_box.currentIndexChanged.connect(self._lane_chosen)
        bar.addWidget(self.lane_box)
        self.root_box = QComboBox()
        self.root_box.addItems(list(scales.NOTE_NAMES))
        self.scale_box = QComboBox()
        self.scale_box.addItems(["No scale", *scales.SCALES])
        self.root_box.currentIndexChanged.connect(self._scale_chosen)
        self.scale_box.currentIndexChanged.connect(self._scale_chosen)
        bar.addWidget(self.root_box)
        bar.addWidget(self.scale_box)
        self.chord_box = QComboBox()
        self.chord_box.addItems([*chords.DIATONIC, *chords.CHORDS])
        self.chord_box.setToolTip("Chord tool shape")
        self.chord_box.currentTextChanged.connect(self._chord_chosen)
        bar.addWidget(self.chord_box)
        bar.addSeparator()
        for label, slot in (
            ("Quantize…", self.quantize_selection),
            ("Humanize…", self.humanize_selection),
            ("Arpeggiate selection", self.arpeggiate_selection),
            ("Riff…", self.generate_riff),
            ("Simplify curve", self.simplify_selection),
        ):
            action = QAction(label, self)
            action.triggered.connect(slot)
            bar.addAction(action)
        ghosts = QAction("Ghosts", self, checkable=True, checked=True)
        ghosts.setToolTip("Show other channels' notes, dimmed")
        ghosts.toggled.connect(self._ghosts_toggled)
        bar.addAction(ghosts)
        follow = QAction("Follow", self, checkable=True, checked=True)
        follow.setToolTip("Scroll with the playhead")
        follow.toggled.connect(self._follow_toggled)
        bar.addAction(follow)
        fit = QAction("Fit", self)
        fit.setToolTip("Zoom to the whole pattern")
        fit.triggered.connect(self.fit_pattern)
        bar.addAction(fit)
        return first, bar

    # --- project and clip -----------------------------------------------------------

    def set_project(
        self, project: Project | None, on_edit: Callable[[], None] | None = None
    ) -> None:
        """Show a project (or nothing). ``on_edit`` runs after every committed command."""
        self.project = project
        self.on_edit = on_edit
        self.pattern_box.clear()
        self.model = None
        if project is None:
            self._clear()
            return
        patterns = project.patterns()
        self.pattern_box.addItems(patterns)
        best = next(
            (p for p in patterns if project.pattern_channels(p)), patterns[0] if patterns else ""
        )
        if best:
            self.pattern_box.setCurrentText(best)
        self._fill_channels()
        self._clip_chosen()

    def _fill_channels(self) -> None:
        if self.project is None:
            return
        pattern = self.pattern_box.currentText()
        playing = self.project.pattern_channels(pattern) if pattern else []
        self.channel_box.clear()
        channels = self.project.channels()
        self.channel_box.addItems(channels)
        if playing:
            self.channel_box.setCurrentText(playing[0])

    def _clip_chosen(self, _index: int = 0) -> None:
        if self.project is None:
            return
        if self.sender() is self.pattern_box:
            self._fill_channels()
        pattern = self.pattern_box.currentText()
        channel = self.channel_box.currentText()
        if not pattern or not channel:
            self._clear()
            return
        self.select_clip(pattern, channel)

    def select_clip(self, pattern: str, channel: str) -> None:
        """Edit ``channel``'s notes in ``pattern``."""
        if self.project is None:
            return
        old = self.model
        self.model = RollModel(
            project=self.project, pattern=pattern, channel=channel, view=self.view
        )
        if old is not None:
            self.model.grid, self.model.snap = old.grid, old.snap
            self.model.scale_root, self.model.scale_mask, self.model.lane = (
                old.scale_root,
                old.scale_mask,
                old.lane,
            )
        else:
            self._grid_chosen(self.grid_box.currentIndex())
            self._scale_chosen()
            self.model.lane = min(self.lane_box.currentIndex(), len(LANES) - 1)
        self.model.on_commit = self._committed
        self.pattern_box.setCurrentText(pattern)
        self.channel_box.setCurrentText(channel)
        voice = bool(self.project.channel_info(channel)["voice"])
        if voice and self.lane_box.currentIndex() != LYRIC_LANE:
            self.lane_box.setCurrentIndex(LYRIC_LANE)
        self.ruler.set_meter(*self.project.meter_at(0))
        self.timeline.set_length(self.project.pattern_length(pattern))
        self._placements = self.project.pattern_placements(pattern)
        self.fit_pattern()

    def _clear(self) -> None:
        self.built = None
        for layer in range(6):
            call(self.item, "upload", layer, ULong(0), 0, ULong(0))
        self.root.setProperty("lyricCells", [])

    # --- geometry -------------------------------------------------------------------

    def rebuild(self, force: bool = False) -> None:
        """Build geometry for the view's range and upload it - only when needed."""
        if self.model is None or self.project is None:
            return
        if not force and not self.view.needs_rebuild(self.built):
            return
        built = self.view.build_range()
        ppt = self.view.pixels_per_tick
        viewport = engine_bridge.make_viewport(
            built.tick_start,
            128.0,
            ppt,
            self.view.pixels_per_semitone,
            (built.tick_end - built.tick_start) * ppt,
            128.0 * self.view.pixels_per_semitone,
        )
        m = self.model
        self.builder.build(
            self.project,
            viewport,
            m.pattern,
            m.channel,
            m.selection,
            lane=min(m.lane, len(LANES) - 1),
            scale_root=m.scale_root,
            scale_mask=m.scale_mask,
            grid_division=m.grid if m.snap else PPQ // 4,
            ghosts=self.ghosts,
        )
        self.upload(range(6))
        self.built = built
        self._refresh_lyrics()

    def upload(self, layers: range | tuple[int, ...]) -> None:
        """Copy layers into the item: one lease (engine call) and one memcpy each."""
        lyric = self.lane_box.currentIndex() == LYRIC_LANE
        for layer in layers:
            with self.builder.view(layer) as v:
                count = 0 if (lyric and layer == 4) else v.vertex_count
                call(self.item, "upload", layer, ULong(v.address), count, ULong(v.revision))

    def apply_view(self) -> None:
        """Move the transform (a Qt call, no engine call); rebuild only if needed."""
        beat_start, world_top, ppb, ppr = self.view.transform()
        call(self.item, "setView", beat_start, world_top, ppb, ppr)
        self.ruler.set_view(self.view.tick_start, self.view.pixels_per_tick)
        self.keyboard.set_view(self.view.pitch_top, self.view.pixels_per_semitone)
        self.timeline.set_window(self.view.tick_start, self.view.tick_end)
        self.rebuild()
        self._refresh_overlays()

    def fit_pattern(self) -> None:
        """Zoom to the whole pattern and centre its notes."""
        if self.model is None:
            return
        self.view.show_ticks(0.0, float(self.model.pattern_length()))
        notes = self.model.notes()
        if len(notes):
            self.view.center_pitch(float(np.median(notes["pitch"])) + 0.5)
        self.apply_view()
        self.rebuild(force=True)

    def _committed(self) -> None:
        """After any command: rebuild, and let the engine catch up."""
        if self.project is not None and self.model is not None:
            self._placements = self.project.pattern_placements(self.model.pattern)
        self.rebuild(force=True)
        self._refresh_overlays()
        if self.on_edit is not None:
            self.on_edit()

    def refresh(self) -> None:
        """The project changed outside the roll (an undo from the menu, say)."""
        if self.model is not None:
            self.model.invalidate()
            self.model.select(self.model.selection)
            self._placements = self.model.project.pattern_placements(self.model.pattern)
        self.rebuild(force=True)
        self._refresh_overlays()

    def _selection_changed(self) -> None:
        """Recolour in place: no rebuild."""
        if self.model is None or self.built is None:
            return
        self.builder.update_selection(self.model.selection)
        self.upload((3, 4))

    # --- overlays and playhead -------------------------------------------------------

    def _refresh_overlays(self) -> None:
        v = self.view
        preview = [
            [v.x_of(t0), v.y_of(p + 1), (t1 - t0) * v.pixels_per_tick, v.pixels_per_semitone]
            for t0, t1, p in self.tool.preview
        ]
        self.root.setProperty("preview", preview)
        band = self.tool.band
        if band is not None:
            call(self.item, "setSelectionRect", *[float(c) for c in band], True)
        else:
            call(self.item, "setSelectionRect", 0.0, 0.0, 0.0, 0.0, False)
        self.root.setProperty("laneTop", v.note_height)
        if self.lane_box.currentIndex() == LYRIC_LANE:
            self._position_lyrics()
        count = len(self.model.selection) if self.model is not None else 0
        grid = self.grid_box.currentText()
        self.root.setProperty("hint", f"{self.tool.label} · grid {grid} · {count} selected")

    def _refresh_lyrics(self) -> None:
        if self.model is None or self.project is None or self.lane_box.currentIndex() != LYRIC_LANE:
            self.root.setProperty("lyricsVisible", False)
            return
        built = self.built or self.view.build_range()
        ids, starts, lengths, lyrics, aliases = self.project.lyric_cells(
            self.model.pattern, self.model.channel, int(built.tick_start), int(built.tick_end)
        )
        order = np.argsort(starts, kind="stable")
        self._lyric_ids = ids[order]
        self._lyric_cells = [
            (int(starts[i]), int(lengths[i]), lyrics[i], aliases[i]) for i in order
        ]
        self.root.setProperty("lyricsVisible", True)
        self._position_lyrics()

    def _position_lyrics(self) -> None:
        cells = getattr(self, "_lyric_cells", [])
        v = self.view
        self.root.setProperty(
            "lyricCells",
            [[v.x_of(s), n * v.pixels_per_tick, text, alias] for s, n, text, alias in cells],
        )

    def frame(self, position_ticks: int, rolling: bool) -> None:
        """The 60 Hz frame: move the playhead node. No engine call happens here."""
        local = -1.0
        for start, end, offset in self._placements:
            if start <= position_ticks < end:
                local = float(position_ticks - start + offset)
                break
        self.item.setProperty("playheadBeats", local / PPQ if local >= 0 else -1.0)
        self.ruler.set_playhead(local)
        self.timeline.set_playhead(local)
        if (
            rolling
            and self.follow
            and local >= 0
            and not (self.view.tick_start <= local < self.view.tick_end)
        ):
            self.view.tick_start = local
            self.apply_view()

    # --- input ----------------------------------------------------------------------

    def on_press(self, p: Pointer) -> None:
        if self.model is None:
            return
        self._lyric_edit.hide()
        if p.button == MIDDLE or (p.button == LEFT and p.has(SHIFT) and p.has(ALT)):
            self._pan_origin = (p.x, p.y)
            return
        before = self.model.selection
        if self.view.in_lane(p.y):
            if self.lane_box.currentIndex() != LYRIC_LANE:
                self.lane_editor.press(self.model, p)
            return
        self.tool.press(self.model, p)
        self._after_input(before)

    def on_move(self, p: Pointer) -> None:
        if self.model is None:
            return
        if self._pan_origin is not None:
            x0, y0 = self._pan_origin
            self.view.pan(x0 - p.x, p.y - y0)
            self._pan_origin = (p.x, p.y)
            self.apply_view()
            return
        if self.lane_editor.active:
            self.lane_editor.move(self.model, p)
            return
        self.tool.move(self.model, p)
        self._refresh_overlays()

    def on_release(self, p: Pointer) -> None:
        if self.model is None:
            return
        if self._pan_origin is not None:
            self._pan_origin = None
            return
        before = self.model.selection
        if self.lane_editor.active:
            self.lane_editor.release(self.model, p)
            return
        self.tool.release(self.model, p)
        self._after_input(before)

    def _after_input(self, before: np.ndarray) -> None:
        if self.model is not None and not np.array_equal(before, self.model.selection):
            self._selection_changed()
        self._refresh_overlays()

    def on_double_click(self, x: float, y: float) -> None:
        if (
            self.model is None
            or not self.view.in_lane(y)
            or self.lane_box.currentIndex() != LYRIC_LANE
        ):
            return
        tick = self.view.tick_at(x)
        for index, (start, length, text, _alias) in enumerate(getattr(self, "_lyric_cells", [])):
            if start <= tick < start + max(length, 1):
                self._edit_lyric(index, text)
                return

    def _edit_lyric(self, index: int, text: str) -> None:
        cells = getattr(self, "_lyric_cells", [])
        if not 0 <= index < len(cells):
            self._lyric_edit.hide()
            return
        start, length, _old, _alias = cells[index]
        self._lyric_note = int(self._lyric_ids[index])
        self._lyric_index = index
        self._lyric_edit.setText(text)
        self._lyric_edit.setGeometry(
            int(self.view.x_of(start)),
            int(self.view.note_height) + 4,
            max(60, int(length * self.view.pixels_per_tick)),
            24,
        )
        self._lyric_edit.show()
        self._lyric_edit.setFocus()
        self._lyric_edit.selectAll()

    def _commit_lyric(self, advance: bool) -> None:
        """One SetLyric command; Tab moves to the next note so a line types straight through."""
        if self.model is None or not self._lyric_note:
            return
        text = self._lyric_edit.text().strip()
        index = getattr(self, "_lyric_index", 0)
        current = self._lyric_cells[index][2] if index < len(self._lyric_cells) else ""
        if text != current:
            self.model.commit(
                engine_bridge.commands.set_lyric(
                    self.model.pattern, self.model.channel, self._lyric_note, text
                )
            )
        self._lyric_note = 0
        if advance and index + 1 < len(self._lyric_cells):
            self._edit_lyric(index + 1, self._lyric_cells[index + 1][2])
        else:
            self._lyric_edit.hide()
            self.quick.setFocus()

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802 - Qt's name
        if watched is self._lyric_edit and event.type() == QEvent.Type.KeyPress:
            assert isinstance(event, QKeyEvent)
            if event.key() == Qt.Key.Key_Tab:
                self._commit_lyric(advance=True)
                return True
            if event.key() == Qt.Key.Key_Escape:
                self._lyric_note = 0
                self._lyric_edit.hide()
                return True
        return super().eventFilter(watched, event)

    def on_hover(self, x: float, y: float) -> None:
        """Hover does nothing yet; the cursor shape is the tool's (Phase 6 polish)."""

    def on_wheel(self, x: float, y: float, dx: float, dy: float, modifiers: int) -> None:
        """Ctrl zooms time, Alt zooms pitch, Shift scrolls time, plain scrolls pitch."""
        steps = dy / 120.0
        if modifiers & CTRL:
            self.view.zoom_time(1.15**steps, x)
        elif modifiers & ALT:
            self.view.zoom_pitch(1.1 ** (steps or dx / 120.0), y)
        elif modifiers & SHIFT or dx:
            self.view.pan(-(steps or dx / 120.0) * 80.0, 0.0)
        else:
            self.view.pan(0.0, steps * self.view.pixels_per_semitone * 3)
        self.apply_view()

    def on_key(self, key: int, modifiers: int) -> None:
        if self.model is None:
            return
        if key == Qt.Key.Key_A and modifiers & CTRL:
            self.model.select(self.model.notes()["id"])
            self._selection_changed()
        elif key == Qt.Key.Key_Escape:
            self.model.select([])
            self._selection_changed()
        elif not self.tool.key(self.model, key, modifiers):
            # Delete and the arrow nudges work whatever the tool.
            if key in (KEY_DELETE, Qt.Key.Key_Backspace) or key in range(0x01000012, 0x01000016):
                SelectTool().key(self.model, key, modifiers)
        self._refresh_overlays()

    def on_resize(self, width: float, height: float) -> None:
        self.view.width = max(1.0, width)
        self.view.height = max(1.0, height)
        self.view.lane_height = min(self.theme.metrics.lane_height, height * 0.35)
        self.item.setProperty("laneHeight", self.view.lane_height)
        self.apply_view()

    def _seek_from_ruler(self, tick: float) -> None:
        if self.seek is not None and len(self._placements):
            start, _end, offset = self._placements[0]
            self.seek(int(start - offset + tick))

    def _scroll_to(self, tick: float) -> None:
        self.view.tick_start = tick
        self.apply_view()

    # --- toolbar --------------------------------------------------------------------

    def set_tool(self, name: str) -> None:
        """Switch tools; a gesture in flight is dropped."""
        self.tool.reset()
        self.tool = self.tools[name]
        if name in self.tool_actions:
            self.tool_actions[name].setChecked(True)
        self._refresh_overlays()

    def _grid_chosen(self, index: int) -> None:
        if self.model is None:
            return
        if 0 <= index < len(quantize.GRIDS):
            self.model.grid = quantize.GRIDS[index][1]
            self.model.snap = True
            self.model.default_length = self.model.grid
        else:
            self.model.snap = False
        self.rebuild(force=True)

    def _lane_chosen(self, index: int) -> None:
        if self.model is not None:
            self.model.lane = min(index, len(LANES) - 1)
        self.rebuild(force=True)
        self._refresh_overlays()

    def _scale_chosen(self, _index: int = 0) -> None:
        root = self.root_box.currentIndex()
        mask = scales.mask_of(self.scale_box.currentText())
        self.keyboard.set_scale(root, mask)
        if self.model is not None:
            self.model.scale_root, self.model.scale_mask = root, mask
        self.rebuild(force=True)

    def _chord_chosen(self, shape: str) -> None:
        tool = self.tools["chord"]
        if isinstance(tool, chords.ChordTool):
            tool.shape = shape

    def _ghosts_toggled(self, on: bool) -> None:
        self.ghosts = on
        self.rebuild(force=True)

    def _follow_toggled(self, on: bool) -> None:
        self.follow = on

    def _targets(self) -> NoteArray:
        assert self.model is not None
        return self.model.selected_notes() if len(self.model.selection) else self.model.notes()

    def quantize_selection(self) -> None:
        """Quantize the selection (or every note): one command."""
        if self.model is None:
            return
        dialog = quantize.QuantizeDialog(self.model.grid, self)
        if dialog.exec():
            rows = quantize.quantize(self._targets(), dialog.settings())
            if len(rows):
                self.model.edit("Quantize", update=rows)

    def humanize_selection(self) -> None:
        """Seeded humanize of the selection (or every note): one command."""
        if self.model is None:
            return
        dialog = quantize.HumanizeDialog(self.humanize_seed, self)
        if dialog.exec():
            settings = dialog.settings()
            self.humanize_seed = settings.seed + 1
            rows = quantize.humanize(self._targets(), settings)
            if len(rows):
                self.model.edit(f"Humanize (seed {settings.seed})", update=rows)

    def arpeggiate_selection(self) -> None:
        """Rewrite the selected chord(s) as a run - a pattern edit, not the channel arp."""
        if self.model is None:
            return
        order, ok = QInputDialog.getItem(
            self, "Arpeggiate selection", "Order", list(chords.ARP_ORDERS), 0, False
        )
        if ok:
            chords.arpeggiate(self.model, order)
            self._selection_changed()

    def generate_riff(self) -> None:
        """A seeded riff from the playhead's bar, over the scale."""
        if self.model is None:
            return
        seed, ok = QInputDialog.getInt(
            self, "Generate riff", "Seed", self.humanize_seed, 0, 2**31 - 1
        )
        if not ok:
            return
        rhythm, ok = QInputDialog.getItem(
            self, "Generate riff", "Rhythm", list(chords.RHYTHMS), 2, False
        )
        if ok:
            bar = PPQ * 4
            start = max(0, int(self.view.tick_start // bar) * bar)
            chords.generate_riff(
                self.model, chords.RiffSettings(seed=seed, bars=2, rhythm=rhythm, start=start)
            )

    def simplify_selection(self) -> None:
        """Thin the selected note's drawn pitch curve."""
        if self.model is not None and len(self.model.selection) == 1:
            simplify_curve(self.model, int(self.model.selection[0]))

    # --- theme and persistence -------------------------------------------------------

    def apply_theme(self, theme: Theme) -> None:
        """Recolour everything; the geometry is untouched (roles resolve at upload)."""
        self.theme = theme
        self.quick.rootContext().setContextProperty("adxTheme", qml_palette(theme))
        self.quick.setClearColor(QColor(theme.window))
        call(self.item, "setPalette", list(role_colors(theme)))
        for widget in (self.ruler, self.keyboard, self.timeline):
            widget.set_theme(theme)

    def write_state(self, settings: QSettings) -> None:
        settings.setValue("tool", self.tool.name)
        settings.setValue("grid", self.grid_box.currentIndex())
        settings.setValue("lane", self.lane_box.currentIndex())
        settings.setValue("scale_root", self.root_box.currentIndex())
        settings.setValue("scale", self.scale_box.currentText())
        settings.setValue("chord", self.chord_box.currentText())

    def read_state(self, settings: QSettings) -> None:
        tool = str(settings.value("tool", "draw"))
        if tool in self.tools:
            self.set_tool(tool)
        self.grid_box.setCurrentIndex(
            int(str(settings.value("grid", self.grid_box.currentIndex())))
        )
        self.lane_box.setCurrentIndex(int(str(settings.value("lane", 0))))
        self.root_box.setCurrentIndex(int(str(settings.value("scale_root", 0))))
        self.scale_box.setCurrentText(str(settings.value("scale", "No scale")))
        self.chord_box.setCurrentText(str(settings.value("chord", "Triad")))
