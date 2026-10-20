"""One mixer strip (phase_6.md 4.3): fader with a dB scale, pan, width, mute/solo/arm,
polarity, output selector, eight effect slots and the send list.

Values on screen come from a fresh read (:meth:`MixerStrip.show_insert`); every change
is a signal the panel turns into a command - a fader or knob drag through
``Session.set_param``, so it is heard at once and lands as one coalesced undo step.
"""

from __future__ import annotations

import math
from typing import Any

from PySide6.QtCore import QMimeData, QPoint, Qt, Signal
from PySide6.QtGui import QDrag, QDragEnterEvent, QDropEvent, QMouseEvent
from PySide6.QtWidgets import (
    QComboBox,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from adx import dnd
from adx.widgets.fader import Fader
from adx.widgets.knob import Knob
from adx.widgets.step_button import StepButton

STRIP_WIDTH = 92
SLOTS = 8
_SLOT_MIME = "application/x-adx-slot"


class SlotRow(QWidget):
    """One effect slot: its effect, bypass, wet/dry. Drag it onto another slot to
    reorder; drop an effect or an effect preset on it to replace it."""

    clicked = Signal(int, int)
    bypassed = Signal(int, bool)
    mix_turned = Signal(int, float)
    moved = Signal(int, int)
    menu_requested = Signal(int, int, QPoint)
    dropped = Signal(int, int, object)

    def __init__(self, index: int, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.index = index
        self.slot = 0
        self.setAcceptDrops(True)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(1)
        self.button = QPushButton("—")
        self.button.setFixedHeight(18)
        self.button.setToolTip("Click: edit (or add) the effect. Drag: reorder.")
        self.button.clicked.connect(lambda: self.clicked.emit(self.index, self.slot))
        self.bypass = StepButton("B", "Bypass")
        self.bypass.setFixedSize(16, 18)
        self.bypass.toggled_by_user.connect(lambda on: self.bypassed.emit(self.slot, on))
        self.mix = Knob(0.0, 1.0, 1.0, 1.0, "")
        self.mix.setFixedSize(18, 18)
        self.mix.setToolTip("Wet/dry")
        self.mix.value_changed.connect(lambda v: self.mix_turned.emit(self.slot, v))
        layout.addWidget(self.button, 1)
        layout.addWidget(self.bypass)
        layout.addWidget(self.mix)
        self._press: QPoint | None = None

    def show_slot(self, slot: dict[str, Any] | None) -> None:
        """Display a fresh read, or an empty slot."""
        self.slot = 0 if slot is None else int(slot["id"])
        self.button.setText("—" if slot is None else str(slot["type"]))
        self.bypass.setVisible(slot is not None)
        self.mix.setVisible(slot is not None)
        if slot is not None:
            self.bypass.set_checked(bool(slot["bypass"]))
            self.mix.set_value(float(slot["mix"]))

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if event.button() == Qt.MouseButton.RightButton:
            self.menu_requested.emit(self.index, self.slot, event.globalPosition().toPoint())
        self._press = event.position().toPoint()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if self._press is None or not self.slot:
            return
        if (event.position().toPoint() - self._press).manhattanLength() < 6:
            return
        drag = QDrag(self)
        data = QMimeData()
        data.setData(_SLOT_MIME, str(self.slot).encode())
        drag.setMimeData(data)
        self._press = None
        drag.exec(Qt.DropAction.MoveAction)

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:  # noqa: N802 - Qt's name
        if event.mimeData().hasFormat(_SLOT_MIME) or dnd.decode(event.mimeData()) is not None:
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent) -> None:  # noqa: N802 - Qt's name
        data = event.mimeData()
        if data.hasFormat(_SLOT_MIME):
            self.moved.emit(int(bytes(data.data(_SLOT_MIME).data()).decode()), self.index)
        else:
            payload = dnd.decode(data)
            if payload is not None:
                self.dropped.emit(self.index, self.slot, payload)
        event.acceptProposedAction()


class MixerStrip(QWidget):
    """An insert's strip. Signals carry the insert id."""

    field_turned = Signal(int, str, float)
    field_set = Signal(int, str, float)
    renamed = Signal(int, str)
    output_chosen = Signal(int, int)
    slot_clicked = Signal(int, int, int)
    slot_bypassed = Signal(int, bool)
    slot_mix = Signal(int, float)
    slot_moved = Signal(int, int, int)
    slot_menu = Signal(int, int, int, QPoint)
    slot_dropped = Signal(int, int, int, object)
    send_turned = Signal(int, float)
    send_removed = Signal(int)
    add_send = Signal(int, QPoint)
    selected = Signal(int)

    def __init__(self, insert: int, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.insert = insert
        self.setFixedWidth(STRIP_WIDTH)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(2, 2, 2, 2)
        layout.setSpacing(2)
        self.name = QLabel()
        self.name.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.name.setToolTip("Double-click to rename")
        layout.addWidget(self.name)
        self.slots: list[SlotRow] = []
        for index in range(SLOTS):
            row = SlotRow(index)
            row.clicked.connect(lambda i, s: self.slot_clicked.emit(self.insert, i, s))
            row.bypassed.connect(self.slot_bypassed)
            row.mix_turned.connect(self.slot_mix)
            row.moved.connect(lambda s, i: self.slot_moved.emit(self.insert, s, i))
            row.menu_requested.connect(lambda i, s, p: self.slot_menu.emit(self.insert, i, s, p))
            row.dropped.connect(
                lambda i, s, payload: self.slot_dropped.emit(self.insert, i, s, payload)
            )
            self.slots.append(row)
            layout.addWidget(row)
        self.sends = QVBoxLayout()
        self.sends.setSpacing(1)
        layout.addLayout(self.sends)
        send_button = QPushButton("+ Send")
        send_button.setFixedHeight(18)
        send_button.clicked.connect(
            lambda: self.add_send.emit(self.insert, send_button.mapToGlobal(QPoint(0, 18)))
        )
        layout.addWidget(send_button)
        knobs = QGridLayout()
        self.pan = Knob(-1.0, 1.0, 0.0, 0.0, "pan")
        self.pan.value_changed.connect(lambda v: self.field_turned.emit(self.insert, "pan", v))
        self.width_knob = Knob(0.0, 2.0, 1.0, 1.0, "width")
        self.width_knob.value_changed.connect(
            lambda v: self.field_turned.emit(self.insert, "width", v)
        )
        knobs.addWidget(self.pan, 0, 0)
        knobs.addWidget(self.width_knob, 0, 1)
        layout.addLayout(knobs)
        buttons = QHBoxLayout()
        self.mute = StepButton("M", "Mute")
        self.solo = StepButton("S", "Solo")
        self.invert = StepButton("Ø", "Invert polarity")
        self.arm = StepButton("R", "Recording arrives in Phase 8 (plans/phase_8.md)")
        self.arm.setEnabled(False)
        for button, field in ((self.mute, "mute"), (self.solo, "solo"), (self.invert, "invert")):
            button.toggled_by_user.connect(
                lambda on, f=field: self.field_set.emit(self.insert, f, 1.0 if on else 0.0)
            )
            buttons.addWidget(button)
        buttons.addWidget(self.arm)
        layout.addLayout(buttons)
        self.fader = Fader(1.0)
        self.fader.setMinimumHeight(120)
        self.fader.value_changed.connect(lambda g: self.field_turned.emit(self.insert, "gain", g))
        layout.addWidget(self.fader, 1)
        self.db = QLabel()
        self.db.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.db)
        self.output = QComboBox()
        self.output.setToolTip("Output: the insert this one routes to")
        self.output.activated.connect(self._output_chosen)
        layout.addWidget(self.output)
        self._targets: list[int] = []

    def show_insert(
        self,
        info: dict[str, Any],
        outputs: list[tuple[int, str]],
        routed_to: int,
        names: dict[int, str],
    ) -> None:
        """Display a fresh read. Emits nothing."""
        master = bool(info["master"])
        self.name.setText(("★ " if master else "") + (str(info["name"]) or f"Insert {info['id']}"))
        slots = list(info["slots"])
        for index, row in enumerate(self.slots):
            row.show_slot(slots[index] if index < len(slots) else None)
        self.fader.set_gain(float(info["gain"]))
        gain = float(info["gain"])
        self.db.setText("-inf dB" if gain <= 0 else f"{20 * math.log10(gain):+.1f} dB")
        self.pan.set_value(float(info["pan"]))
        self.width_knob.set_value(float(info["width"]))
        self.mute.set_checked(bool(info["muted"]))
        self.solo.set_checked(bool(info["soloed"]))
        self.invert.set_checked(bool(info["invert"]))
        targets = [0, *[i for i, _n in outputs]]
        if targets != self._targets:
            self.output.blockSignals(True)
            self.output.clear()
            self.output.addItem("(master out)" if master else "(none)")
            for insert, name in outputs:
                self.output.addItem(f"→ {insert} {name}")
            self.output.blockSignals(False)
            self._targets = targets
        self.output.blockSignals(True)
        self.output.setCurrentIndex(targets.index(routed_to) if routed_to in targets else 0)
        self.output.blockSignals(False)
        self._show_sends(list(info["sends"]), names)

    def _show_sends(self, sends: list[dict[str, Any]], names: dict[int, str]) -> None:
        while self.sends.count():
            item = self.sends.takeAt(0)
            widget = item.widget() if item is not None else None
            if widget is not None:
                widget.deleteLater()
        for send in sends:
            row = QWidget()
            line = QHBoxLayout(row)
            line.setContentsMargins(0, 0, 0, 0)
            label = QLabel(f"→{names.get(int(send['target']), send['target'])}")
            label.setToolTip("Send (right-click to remove)")
            level = Knob(0.0, 1.0, float(send["level"]), 0.5, "")
            level.setFixedSize(18, 18)
            level.setToolTip("Send level")
            send_id = int(send["id"])
            level.value_changed.connect(lambda v, s=send_id: self.send_turned.emit(s, v))
            label.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            label.customContextMenuRequested.connect(
                lambda _p, s=send_id: self.send_removed.emit(s)
            )
            line.addWidget(label, 1)
            line.addWidget(level)
            self.sends.addWidget(row)

    def _output_chosen(self, index: int) -> None:
        if 0 <= index < len(self._targets):
            self.output_chosen.emit(self.insert, self._targets[index])

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if event.button() == Qt.MouseButton.LeftButton:
            self.selected.emit(self.insert)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802 - Qt's name
        if event.position().y() > self.name.height() + 4:
            return
        from PySide6.QtWidgets import QInputDialog

        name, ok = QInputDialog.getText(
            self, "Rename insert", "Name", text=self.name.text().lstrip("★ ")
        )
        if ok:
            self.renamed.emit(self.insert, name.strip())


def effect_menu(effects: list[str], parent: QWidget | None = None) -> QMenu:
    """A menu of effect types, each action carrying its name as data."""
    menu = QMenu(parent)
    for name in effects:
        action = menu.addAction(name)
        action.setData(name)
    return menu
