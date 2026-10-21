"""One channel rack row: name, colour, mute/solo, level and pan, output insert,
polyphony, and the instrument button (phase_6.md 4.2).

The row holds widgets and the id of the channel it shows - view state only. Every value
it displays is set by :meth:`ChannelRow.show_channel` from a fresh read, and every edit it
makes is a signal the panel turns into a command.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QDragEnterEvent, QDropEvent, QMouseEvent
from PySide6.QtWidgets import QComboBox, QHBoxLayout, QLabel, QPushButton, QSpinBox, QWidget

from adx import dnd
from adx.widgets.color_swatch import ColorSwatch
from adx.widgets.knob import Knob
from adx.widgets.step_button import StepButton

#: Every row is this tall, and the step grid's rows match it.
ROW_HEIGHT = 30
#: The column header above the rows (and the gap above the step grid).
HEADER_HEIGHT = 18

#: (caption, width, tooltip) per column, left to right - the header and every row
#: lay their widgets out at these widths so the captions line up.
COLUMNS: tuple[tuple[str, int, str], ...] = (
    ("", 14, "Colour"),
    ("M", 24, "Mute"),
    ("S", 24, "Solo"),
    ("Vol", 26, "Volume"),
    ("Pan", 26, "Pan"),
    ("Channel", 96, "Click to select (its notes load in the piano roll); double-click to rename"),
    ("Output", 84, "The mixer insert this channel plays into"),
    ("Voices", 58, "Polyphony: notes it can sound at once"),
    ("Instrument", 76, "Open the instrument editor"),
    ("", 64, ""),
)


class ChannelRow(QWidget):
    """A channel's controls. Signals carry the channel id."""

    field_changed = Signal(int, str, float)
    renamed = Signal(int, str)
    color_changed = Signal(int, str)
    output_changed = Signal(int, int)
    selected = Signal(int)
    open_instrument = Signal(int)
    #: The Notes button: edit this channel's notes in the piano roll.
    open_notes = Signal(int)
    dropped = Signal(int, object)

    def __init__(self, channel: int, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.channel = channel
        self.setFixedHeight(ROW_HEIGHT)
        self.setAcceptDrops(True)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(2, 1, 2, 1)
        layout.setSpacing(3)
        self.swatch = ColorSwatch()
        self.swatch.color_chosen.connect(lambda c: self.color_changed.emit(self.channel, c))
        self.mute = StepButton("M", "Mute", kind="mute")
        self.mute.toggled_by_user.connect(
            lambda on: self.field_changed.emit(self.channel, "mute", 1.0 if on else 0.0)
        )
        self.solo = StepButton("S", "Solo", kind="solo")
        self.solo.toggled_by_user.connect(
            lambda on: self.field_changed.emit(self.channel, "solo", 1.0 if on else 0.0)
        )
        self.volume = Knob(0.0, 2.0, 1.0, 1.0, "")
        self.volume.setFixedSize(26, 26)
        self.volume.setToolTip("Volume")
        self.volume.value_changed.connect(
            lambda v: self.field_changed.emit(self.channel, "volume", v)
        )
        self.pan = Knob(-1.0, 1.0, 0.0, 0.0, "")
        self.pan.setFixedSize(26, 26)
        self.pan.setToolTip("Pan")
        self.pan.value_changed.connect(lambda v: self.field_changed.emit(self.channel, "pan", v))
        self.name = QLabel()
        self.name.setFixedWidth(96)
        self.name.setToolTip("Click to select; double-click to rename")
        self.output = QComboBox()
        self.output.setFixedWidth(84)
        self.output.setToolTip("Output insert")
        self.output.activated.connect(self._output_chosen)
        self.polyphony = QSpinBox()
        self.polyphony.setRange(1, 256)
        self.polyphony.setFixedWidth(58)
        self.polyphony.setButtonSymbols(QSpinBox.ButtonSymbols.NoButtons)
        self.polyphony.setToolTip("Polyphony")
        self.polyphony.setKeyboardTracking(False)
        self.polyphony.valueChanged.connect(
            lambda v: self.field_changed.emit(self.channel, "polyphony", float(v))
        )
        self.instrument = QPushButton()
        self.instrument.setFixedWidth(76)
        self.instrument.setToolTip("Open the instrument editor")
        self.instrument.clicked.connect(lambda: self.open_instrument.emit(self.channel))
        self.notes = QPushButton("Notes")
        self.notes.setFixedWidth(64)
        self.notes.setToolTip("Edit this channel's notes in the piano roll (F7)")
        self.notes.clicked.connect(lambda: self.open_notes.emit(self.channel))
        for widget in (
            self.swatch,
            self.mute,
            self.solo,
            self.volume,
            self.pan,
            self.name,
            self.output,
            self.polyphony,
            self.instrument,
            self.notes,
        ):
            layout.addWidget(widget)
        self._inserts: list[int] = []

    def show_channel(self, info: dict[str, Any], inserts: list[tuple[int, str]]) -> None:
        """Display a fresh read. Emits nothing."""
        self.swatch.set_color(str(info["color"]))
        self.mute.set_checked(bool(info["muted"]))
        self.solo.set_checked(bool(info["soloed"]))
        self.volume.set_value(float(info["volume"]))
        self.pan.set_value(float(info["pan"]))
        self.name.setText(str(info["name"]))
        known = "" if info["known"] else " (?)"
        self.instrument.setText(f"{info['instrument']}{known}")
        ids = [i for i, _n in inserts]
        if ids != self._inserts:
            self.output.blockSignals(True)
            self.output.clear()
            for insert, name in inserts:
                self.output.addItem(f"{insert} {name}".strip())
            self.output.blockSignals(False)
            self._inserts = ids
        output = int(info["output"])
        if output in self._inserts:
            self.output.blockSignals(True)
            self.output.setCurrentIndex(self._inserts.index(output))
            self.output.blockSignals(False)
        self.polyphony.blockSignals(True)
        self.polyphony.setValue(int(info["polyphony"]))
        self.polyphony.blockSignals(False)

    @staticmethod
    def header() -> QWidget:
        """Captions for the row's columns, laid out at the row's widths."""
        header = QWidget()
        header.setFixedHeight(HEADER_HEIGHT)
        layout = QHBoxLayout(header)
        layout.setContentsMargins(2, 0, 2, 0)
        layout.setSpacing(3)
        for caption, width, tooltip in COLUMNS:
            label = QLabel(caption)
            label.setObjectName("columnHeader")
            label.setFixedWidth(width)
            label.setToolTip(tooltip)
            label.setAlignment(Qt.AlignmentFlag.AlignCenter)
            layout.addWidget(label)
        return header

    def _output_chosen(self, index: int) -> None:
        if 0 <= index < len(self._inserts):
            self.output_changed.emit(self.channel, self._inserts[index])

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if event.button() == Qt.MouseButton.LeftButton:
            self.selected.emit(self.channel)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        from PySide6.QtWidgets import QInputDialog

        name, ok = QInputDialog.getText(self, "Rename channel", "Name", text=self.name.text())
        if ok and name.strip() and name != self.name.text():
            self.renamed.emit(self.channel, name.strip())

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:  # noqa: N802 - Qt's name
        payload = dnd.decode(event.mimeData())
        if payload is not None and dnd.accepts(payload, dnd.ChannelTarget(self.channel)):
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent) -> None:  # noqa: N802 - Qt's name
        payload = dnd.decode(event.mimeData())
        if payload is not None:
            self.dropped.emit(self.channel, payload)
            event.acceptProposedAction()
