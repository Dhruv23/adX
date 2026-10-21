"""The channel rack: every channel, and a step sequencer over the selected pattern
(phase_6.md 4.2). The first panel in Phase 6's build order - it is where channels are made.

Layout: a toolbar (pattern, steps, step size, swing, add channel), then one scrolling
body holding the channel rows on the left and the C++ step grid on the right, row for row.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QDragEnterEvent, QDropEvent
from PySide6.QtWidgets import (
    QComboBox,
    QHBoxLayout,
    QInputDialog,
    QScrollArea,
    QSlider,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from adx import dnd, engine_bridge
from adx.engine_bridge import PPQ
from adx.panels.base import DockPanel
from adx.panels.channel_rack.channel_row import HEADER_HEIGHT, ChannelRow
from adx.panels.channel_rack.step_grid import StepGrid
from adx.panels.params import channel_path
from adx.selection import Context, Kind, Selection
from adx.session import Session
from adx.theme.tokens import Theme

STEP_COUNTS = (16, 32, 64)
STEP_SIZES = (("1/16", PPQ // 4), ("1/8", PPQ // 2), ("1/32", PPQ // 8), ("1/64", PPQ // 16))


class _Body(QWidget):
    """The rows and the grid, taking drops on empty space (a new channel)."""

    dropped = Signal(object)

    def __init__(self) -> None:
        super().__init__()
        self.setAcceptDrops(True)

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:  # noqa: N802 - Qt's name
        payload = dnd.decode(event.mimeData())
        if payload is not None and dnd.accepts(payload, dnd.RackTarget()):
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent) -> None:  # noqa: N802 - Qt's name
        payload = dnd.decode(event.mimeData())
        if payload is not None:
            self.dropped.emit(payload)
            event.acceptProposedAction()


class ChannelRackPanel(DockPanel):
    """Channels, and the selected pattern's steps."""

    panel_id = "channel_rack"
    title = "Channel rack"
    default_area = Qt.DockWidgetArea.RightDockWidgetArea

    #: (pattern name, channel name): open the piano roll on that clip.
    open_roll = Signal(str, str)
    #: A channel id: show its instrument editor.
    open_instrument = Signal(int)
    #: (pattern name, channel name): open that clip in the piano roll and show it.
    edit_notes = Signal(str, str)

    def __init__(
        self, session: Session, selection: Selection, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.selection = selection
        self.grid = StepGrid(session, self._audition)
        self.rows: list[ChannelRow] = []
        body = QWidget()
        outer = QVBoxLayout(body)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(self._build_toolbar())
        self.scroller = QScrollArea()
        self.scroller.setWidgetResizable(True)
        self.content = _Body()
        self.content.dropped.connect(self._dropped_on_rack)
        content_layout = QHBoxLayout(self.content)
        content_layout.setContentsMargins(0, 0, 0, 0)
        content_layout.setSpacing(4)
        rows_column = QVBoxLayout()
        rows_column.setSpacing(0)
        rows_column.setContentsMargins(0, 0, 0, 0)
        rows_column.addWidget(ChannelRow.header())
        self.row_column = QVBoxLayout()
        self.row_column.setSpacing(0)
        self.row_column.setContentsMargins(0, 0, 0, 0)
        rows_column.addLayout(self.row_column)
        rows_widget = QWidget()
        rows_widget.setLayout(rows_column)
        content_layout.addWidget(rows_widget, 0, Qt.AlignmentFlag.AlignTop)
        grid_column = QVBoxLayout()
        grid_column.setContentsMargins(0, 0, 0, 0)
        grid_column.addSpacing(HEADER_HEIGHT)
        grid_column.addWidget(self.grid.view)
        grid_column.addStretch(1)
        content_layout.addLayout(grid_column)
        content_layout.addStretch(1)
        self.scroller.setWidget(self.content)
        outer.addWidget(self.scroller, 1)
        self.setWidget(body)
        session.changed.connect(self.refresh)

    def _build_toolbar(self) -> QToolBar:
        bar = QToolBar()
        self.pattern_box = QComboBox()
        self.pattern_box.setToolTip("The pattern the steps edit")
        self.pattern_box.setMinimumWidth(110)
        self.pattern_box.activated.connect(lambda _i: self.refresh())
        bar.addWidget(self.pattern_box)
        add_pattern = bar.addAction("+ Pattern")
        add_pattern.triggered.connect(self.add_pattern)
        bar.addSeparator()
        self.steps_box = QComboBox()
        self.steps_box.addItems([str(n) for n in STEP_COUNTS])
        self.steps_box.setToolTip("Steps")
        self.steps_box.activated.connect(lambda _i: self.refresh())
        bar.addWidget(self.steps_box)
        self.size_box = QComboBox()
        self.size_box.addItems([label for label, _t in STEP_SIZES])
        self.size_box.setToolTip("Step length")
        self.size_box.activated.connect(lambda _i: self.refresh())
        bar.addWidget(self.size_box)
        self.swing = QSlider(Qt.Orientation.Horizontal)
        self.swing.setRange(0, 75)
        self.swing.setFixedWidth(80)
        self.swing.setToolTip("Swing: every second step written up to half a step late")
        self.swing.valueChanged.connect(lambda v: setattr(self.grid.state, "swing", v / 100.0))
        bar.addWidget(self.swing)
        bar.addSeparator()
        self.instrument_box = QComboBox()
        self.instrument_box.setToolTip("Instrument for a new channel")
        bar.addWidget(self.instrument_box)
        add = bar.addAction("+ Channel")
        add.triggered.connect(self.add_channel)
        return bar

    # --- reading --------------------------------------------------------------------

    def refresh(self) -> None:
        """Re-read everything shown from the engine: channels, patterns, steps."""
        project = self.session.project
        if project is None:
            return
        patterns = project.patterns_info()
        names = [str(p["name"]) for p in patterns]
        current = self.pattern_box.currentText()
        if [self.pattern_box.itemText(i) for i in range(self.pattern_box.count())] != names:
            self.pattern_box.clear()
            self.pattern_box.addItems(names)
        if current in names:
            self.pattern_box.setCurrentText(current)
        if self.instrument_box.count() == 0:
            # The catalog is the engine's static type table, not project state.
            catalog = [t for t in engine_bridge.type_catalog() if t["kind"] == "instrument"]
            self.instrument_box.addItems([str(t["name"]) for t in catalog])
        channels = project.channels_info()
        mixer = project.mixer_info()
        inserts = [(int(i["id"]), str(i["name"])) for i in mixer["inserts"]]
        self._sync_rows(channels, inserts)
        pattern = next((p for p in patterns if p["name"] == self.pattern_box.currentText()), None)
        state = self.grid.state
        state.pattern = "" if pattern is None else str(pattern["name"])
        state.pattern_id = 0 if pattern is None else int(pattern["id"])
        state.channels = tuple((int(c["id"]), str(c["name"])) for c in channels)
        state.steps = STEP_COUNTS[max(0, self.steps_box.currentIndex())]
        state.step_ticks = STEP_SIZES[max(0, self.size_box.currentIndex())][1]
        self.grid.rebuild()

    def _sync_rows(self, channels: list[dict[str, Any]], inserts: list[tuple[int, str]]) -> None:
        ids = [int(c["id"]) for c in channels]
        if [row.channel for row in self.rows] != ids:
            # Incremental: a row is a widget tree, and rebuilding every row for each
            # added channel made adding N channels cost O(N^2) widgets.
            existing = {row.channel: row for row in self.rows}
            for channel, row in existing.items():
                if channel not in ids:
                    row.setParent(None)
                    row.deleteLater()
            self.rows = []
            for channel in ids:
                row = existing.get(channel) or ChannelRow(channel)
                if channel not in existing:
                    self._wire_row(row)
                self.rows.append(row)
            for index, row in enumerate(self.rows):
                self.row_column.insertWidget(index, row)
        for row, info in zip(self.rows, channels, strict=True):
            row.show_channel(info, inserts)

    def _wire_row(self, row: ChannelRow) -> None:
        row.field_changed.connect(self._field_changed)
        row.renamed.connect(lambda c, n: self.session.build_and_execute("RenameChannelId", c, n))
        row.color_changed.connect(
            lambda c, color: self.session.build_and_execute("SetChannelColor", c, color)
        )
        row.output_changed.connect(
            lambda c, i: self.session.build_and_execute("SetChannelOutput", c, i)
        )
        row.selected.connect(self._row_selected)
        row.open_instrument.connect(self.open_instrument)
        row.open_notes.connect(self._open_notes)
        row.dropped.connect(self._dropped_on_channel)

    # --- edits ----------------------------------------------------------------------

    def _channel_name(self, channel: int) -> str:
        return next((n for c, n in self.grid.state.channels if c == channel), "")

    def _field_changed(self, channel: int, field: str, value: float) -> None:
        if field in ("volume", "pan"):
            self.session.set_param(channel_path(self._channel_name(channel), field), value)
        else:
            self.session.build_and_execute("SetChannelField", channel, field, value)

    def _row_selected(self, channel: int) -> None:
        self.selection.select(Kind.CHANNELS, [channel], Context())
        if self.grid.state.pattern:
            self.open_roll.emit(self.grid.state.pattern, self._channel_name(channel))

    def _open_notes(self, channel: int) -> None:
        name = self._channel_name(channel)
        self.selection.select(Kind.CHANNELS, [channel], Context())
        if self.grid.state.pattern and name:
            self.edit_notes.emit(self.grid.state.pattern, name)

    def add_channel(self) -> None:
        """A new channel with the chosen instrument, feeding the master."""
        project = self.session.require()
        taken = {str(c["name"]) for c in project.channels_info()}
        n = len(taken) + 1
        while f"Channel{n}" in taken:
            n += 1
        instrument = self.instrument_box.currentText() or "additive"
        self.session.build_and_execute("AddChannelNew", f"Channel{n}", instrument, 0, "")

    def add_pattern(self) -> None:
        """A new empty pattern, selected here."""
        project = self.session.require()
        taken = {str(p["name"]) for p in project.patterns_info()}
        n = len(taken) + 1
        while f"P{n}" in taken:
            n += 1
        name, ok = QInputDialog.getText(self, "New pattern", "Name", text=f"P{n}")
        if ok and name.strip():
            self.create_pattern(name.strip())

    def create_pattern(self, name: str, length: int = PPQ * 16) -> bool:
        """Add a pattern and select it."""
        if self.session.build_and_execute("AddPatternNew", name, length, ""):
            self.pattern_box.addItem(name)
            self.pattern_box.setCurrentText(name)
            self.refresh()
            return True
        return False

    def select_pattern(self, name: str) -> None:
        """Edit ``name``'s steps."""
        self.pattern_box.setCurrentText(name)
        self.refresh()

    def _dropped_on_channel(self, channel: int, payload: dnd.Payload) -> None:
        dnd.perform(self.session, payload, dnd.ChannelTarget(channel))

    def _dropped_on_rack(self, payload: dnd.Payload) -> None:
        dnd.perform(self.session, payload, dnd.RackTarget())

    def _audition(self, channel: int, pitch: int) -> None:
        self.session.engine.audition(channel, pitch, 100, 0.25)

    def apply_theme(self, theme: Theme) -> None:
        """Recolour the grid."""
        self.grid.view.apply_theme(theme)
