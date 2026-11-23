"""Controllers: automation sources rather than drawn curves (phase_6.md 4.5).

An LFO, an envelope (retriggered by a channel's notes), a peak follower (an insert's
level), a formula (an expression of position), a keyboard controller (a channel's keys or
velocities) or an X-Y pad drives a parameter - or a macro, and through it everything the
macro moves. They are engine-side - most compile into automation lanes, the peak follower
is a graph node, the pad folds into its targets - so this is only their settings: add,
edit, remove, each one command.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QStringListModel, Signal
from PySide6.QtWidgets import (
    QComboBox,
    QCompleter,
    QFormLayout,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from adx import engine_bridge
from adx.engine_bridge import PPQ
from adx.panels.params import channel_path, slot_path
from adx.session import Session
from adx.widgets.knob import Knob
from adx.widgets.numeric_drag import NumericDrag
from adx.widgets.xy_pad import XYPad

KINDS = ("lfo", "envelope", "peak", "formula", "keyboard", "xy")
KIND_LABELS = {
    "lfo": "LFO",
    "envelope": "Envelope",
    "peak": "Peak",
    "formula": "Formula",
    "keyboard": "Keyboard",
    "xy": "X-Y pad",
}
SHAPES = ("sine", "triangle", "saw", "square", "saw-down", "random", "smooth-random")
PERIODS = (
    ("1/16", PPQ // 4),
    ("1/8", PPQ // 2),
    ("1/4", PPQ),
    ("1/2", PPQ * 2),
    ("1 bar", PPQ * 4),
    ("2 bars", PPQ * 8),
    ("4 bars", PPQ * 16),
)
INPUTS = ("pitch", "velocity")
FORMULA_HELP = (
    "t beats, s seconds, p 0..1 through the period, a b c the knobs. "
    "sin cos tri saw sq rand lerp smoothstep clamp min max ... and ?: for conditions."
)
DEFAULT_FORMULA = "sin(2 * pi * p)"


def parameter_paths(session: Session) -> list[str]:
    """Every parameter path the project has, for the target field's completion."""
    project = session.require()
    catalog = {str(t["name"]): t for t in engine_bridge.type_catalog()}
    paths: list[str] = []
    for channel in project.channels_info():
        name = str(channel["name"])
        paths += [channel_path(name, leaf) for leaf in ("volume", "pan", "pitch")]
        paths += [channel_path(name, str(p[0])) for p in channel["params"]]
    for insert in project.mixer_info()["inserts"]:
        number = int(insert["id"])
        paths += [f"insert.{number}.{leaf}" for leaf in ("gain", "pan", "width")]
        for slot in insert["slots"]:
            paths.append(slot_path(number, int(slot["id"]), "mix"))
            entry = catalog.get(str(slot["type"]))
            if entry is not None:
                paths += [
                    slot_path(number, int(slot["id"]), str(p["name"]))
                    for p in entry["params"]
                    if int(p["curve_part"]) == 0
                ]
    # A macro is a target too: the controller then moves everything the macro does.
    paths += [f"macro.{_segment(str(m['name']))}" for m in project.macros()]
    for controller in project.controllers_info():
        if str(controller["kind"]) == "xy":
            name = _segment(str(controller["name"]))
            paths += [f"controller.{name}.x", f"controller.{name}.y"]
    return paths


def _segment(name: str) -> str:
    bare = name and all(c.isalnum() or c in "_-" for c in name)
    return name if bare else '"' + name.replace("\\", "\\\\").replace('"', '\\"') + '"'


class ControllerEditor(QWidget):
    """The list of controllers and the selected one's settings."""

    changed = Signal()

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.session = session
        layout = QHBoxLayout(self)
        left = QVBoxLayout()
        self.list = QListWidget()
        self.list.currentRowChanged.connect(lambda _r: self.refresh())
        left.addWidget(self.list, 1)
        buttons = QGridLayout()
        for i, kind in enumerate(KINDS):
            button = QPushButton(f"+ {KIND_LABELS[kind]}")
            button.setObjectName(f"add_{kind}")
            button.clicked.connect(lambda _c=False, k=kind: self.add(k))
            buttons.addWidget(button, i // 3, i % 3)
        remove = QPushButton("Remove")
        remove.clicked.connect(self.remove)
        buttons.addWidget(remove, 2, 0, 1, 3)
        left.addLayout(buttons)
        layout.addLayout(left, 1)
        self.form_host = QWidget()
        self.form = QFormLayout(self.form_host)
        layout.addWidget(self.form_host, 2)
        self._ids: list[int] = []
        #: The X-Y pad in the form, while one is shown.
        self.pad: XYPad | None = None
        self._dragging = False

    # --- reading --------------------------------------------------------------------

    def current(self) -> dict[str, Any] | None:
        """The selected controller, read now."""
        row = self.list.currentRow()
        if not 0 <= row < len(self._ids) or self.session.project is None:
            return None
        return next(
            (c for c in self.session.project.controllers_info() if int(c["id"]) == self._ids[row]),
            None,
        )

    def refresh(self) -> None:
        """Re-read the controllers and rebuild the form for the selected one."""
        if self.session.project is None:
            return
        controllers = self.session.project.controllers_info()
        ids = [int(c["id"]) for c in controllers]
        labels = [_label(c) for c in controllers]
        shown = [self.list.item(i).text() for i in range(self.list.count())]
        if ids != self._ids or shown != labels:
            row = self.list.currentRow()
            self.list.blockSignals(True)
            self.list.clear()
            self.list.addItems(labels)
            self.list.blockSignals(False)
            self._ids = ids
            self.list.setCurrentRow(min(max(row, 0), len(ids) - 1))
        if self._dragging:
            # The pad being dragged is in the form: rebuilding it would drop the drag.
            return
        self._build_form(self.current())

    def _build_form(self, controller: dict[str, Any] | None) -> None:
        self.pad = None
        while self.form.rowCount():
            self.form.removeRow(0)
        if controller is None:
            return
        cid = int(controller["id"])
        kind = str(controller["kind"])
        self._target_row(cid, "X target" if kind == "xy" else "Target", "target", controller)
        builder = {
            "lfo": self._lfo_form,
            "envelope": self._source_form,
            "peak": self._source_form,
            "formula": self._formula_form,
            "keyboard": self._keyboard_form,
            "xy": self._xy_form,
        }.get(kind)
        if builder is not None:
            builder(cid, kind, controller)
        if kind != "xy":
            self._knob(cid, "depth", controller, -1.0, 1.0)
            self._knob(cid, "center", controller, 0.0, 1.0)

    def _target_row(self, cid: int, label: str, key: str, controller: dict[str, Any]) -> None:
        field = QLineEdit(str(controller[key]))
        field.setObjectName(key)
        field.setCompleter(QCompleter(QStringListModel(parameter_paths(self.session), field)))
        field.editingFinished.connect(lambda: self.set(cid, {key: field.text().strip()}))
        self.form.addRow(label, field)

    def _lfo_form(self, cid: int, _kind: str, controller: dict[str, Any]) -> None:
        shape = QComboBox()
        shape.addItems(list(SHAPES))
        shape.setCurrentText(str(controller["shape"]))
        shape.activated.connect(lambda i: self.set(cid, {"shape": SHAPES[i]}))
        self.form.addRow("Shape", shape)
        self._period_row(cid, controller)
        self._knob(cid, "phase", controller, 0.0, 1.0)
        if str(controller["shape"]) in ("random", "smooth-random"):
            seed = QSpinBox()
            seed.setRange(0, 2**31 - 1)
            seed.setValue(int(controller["seed"]))
            seed.editingFinished.connect(lambda: self.set(cid, {"seed": seed.value()}))
            self.form.addRow("Seed", seed)

    def _period_row(self, cid: int, controller: dict[str, Any]) -> None:
        period = QComboBox()
        period.addItems([label for label, _t in PERIODS])
        ticks = [t for _l, t in PERIODS]
        current = int(controller["period"])
        period.setCurrentIndex(ticks.index(current) if current in ticks else 2)
        period.activated.connect(lambda i: self.set(cid, {"period": ticks[i]}))
        self.form.addRow("Period", period)

    def _channel_source(self, cid: int, controller: dict[str, Any]) -> None:
        project = self.session.require()
        choices = [(int(c["id"]), str(c["name"])) for c in project.channels_info()]
        self._source_combo(cid, "source_channel", int(controller["source_channel"]), choices)

    def _source_combo(
        self, cid: int, key: str, current: int, choices: list[tuple[int, str]]
    ) -> None:
        source = QComboBox()
        for _i, label in choices:
            source.addItem(label)
        ids = [i for i, _l in choices]
        if current in ids:
            source.setCurrentIndex(ids.index(current))
        source.activated.connect(lambda i: self.set(cid, {key: ids[i]}))
        self.form.addRow("Source", source)

    def _source_form(self, cid: int, kind: str, controller: dict[str, Any]) -> None:
        project = self.session.require()
        if kind == "envelope":
            self._channel_source(cid, controller)
        else:
            choices = [(int(i["id"]), str(i["name"])) for i in project.mixer_info()["inserts"]]
            self._source_combo(cid, "source_insert", int(controller["source_insert"]), choices)
        for field, high in (("attack", 2000.0), ("decay", 5000.0), ("release", 5000.0)):
            if kind == "peak" and field == "decay":
                continue
            self._ms_row(cid, field, controller, high)
        if kind == "envelope":
            self._knob(cid, "sustain", controller, 0.0, 1.0)
        else:
            self._knob(cid, "gain", controller, -24.0, 24.0)

    def _ms_row(self, cid: int, field: str, controller: dict[str, Any], high: float) -> None:
        drag = NumericDrag(
            0.0,
            high,
            float(controller[field]),
            label=field,
            formatter=lambda v: f"{v:.0f} ms",
        )
        drag.edit_finished.connect(lambda v, f=field: self.set(cid, {f: float(v)}))
        self.form.addRow(field.title(), drag)

    def _formula_form(self, cid: int, _kind: str, controller: dict[str, Any]) -> None:
        text = QLineEdit(str(controller["formula"]))
        text.setObjectName("formula")
        text.setPlaceholderText(DEFAULT_FORMULA)
        text.setToolTip(FORMULA_HELP)
        text.editingFinished.connect(lambda: self.set(cid, {"formula": text.text().strip()}))
        self.form.addRow("Formula", text)
        error = str(controller.get("formula_error", ""))
        status = QLabel(error or FORMULA_HELP)
        status.setObjectName("formula_status")
        status.setWordWrap(True)
        status.setProperty("role", "error" if error else "dim")
        self.form.addRow("", status)
        self._period_row(cid, controller)
        for name in ("a", "b", "c"):
            self._knob(cid, name, controller, 0.0, 1.0)

    def _keyboard_form(self, cid: int, _kind: str, controller: dict[str, Any]) -> None:
        self._channel_source(cid, controller)
        source = QComboBox()
        source.addItems(["Key", "Velocity"])
        source.setCurrentIndex(INPUTS.index(str(controller["input"])))
        source.activated.connect(lambda i: self.set(cid, {"input": INPUTS[i]}))
        self.form.addRow("Reads", source)
        for field in ("low", "high"):
            box = QSpinBox()
            box.setObjectName(field)
            box.setRange(0, 127)
            box.setValue(int(controller[field]))
            box.editingFinished.connect(lambda f=field, b=box: self.set(cid, {f: b.value()}))
            self.form.addRow(field.title(), box)
        self._ms_row(cid, "glide", controller, 2000.0)

    def _xy_form(self, cid: int, _kind: str, controller: dict[str, Any]) -> None:
        self._target_row(cid, "Y target", "target_y", controller)
        pad = XYPad(float(controller["x"]), float(controller["y"]))
        pad.setMinimumSize(160, 160)
        pad.moved.connect(lambda x, y: self._pad_moved(cid, x, y))
        pad.edit_finished.connect(lambda x, y: self._pad_released(cid, x, y))
        self.form.addRow(pad)
        self.pad = pad

    def _pad_moved(self, cid: int, x: float, y: float) -> None:
        if not self._dragging:
            # A drag is one undo step: nothing before it coalesces into it.
            self.session.seal()
            self._dragging = True
        self.session.build_and_execute("SetController", cid, {"x": float(x), "y": float(y)})

    def _pad_released(self, cid: int, x: float, y: float) -> None:
        self.session.build_and_execute("SetController", cid, {"x": float(x), "y": float(y)})
        self._dragging = False
        self.session.seal()
        self.refresh()

    def _knob(
        self, cid: int, field: str, controller: dict[str, Any], low: float, high: float
    ) -> None:
        knob = Knob(low, high, float(controller[field]), label=field)
        knob.edit_finished.connect(lambda v: self.set(cid, {field: float(v)}))
        self.form.addRow(field.title(), knob)

    # --- edits ----------------------------------------------------------------------

    def set(self, controller: int, fields: dict[str, Any]) -> None:
        """One SetController: consecutive edits of one controller coalesce."""
        project = self.session.require()
        for key in ("target", "target_y"):
            path = fields.get(key)
            if path is not None and path != "" and not project.param_valid(str(path)):
                self.session.failed.emit("controller", f"{path} does not name a parameter")
                return
        self.session.build_and_execute("SetController", controller, fields)

    def add(self, kind: str, target: str = "") -> bool:
        """A new controller of ``kind`` on ``target`` (the first channel's volume when
        none is given): one command."""
        project = self.session.require()
        channels = project.channels_info()
        if not target and channels:
            target = channel_path(str(channels[0]["name"]), "volume")
        taken = {str(c["name"]) for c in project.controllers_info()}
        base = "XY" if kind == "xy" else kind.title()
        n = 1
        while f"{base}{n}" in taken:
            n += 1
        fields: dict[str, Any] = {"name": f"{base}{n}", "kind": kind, "target": target}
        if kind in ("envelope", "keyboard") and channels:
            fields["source_channel"] = int(channels[0]["id"])
        if kind == "peak":
            fields["source_insert"] = int(project.mixer_info()["master"])
        if kind == "formula":
            fields["formula"] = DEFAULT_FORMULA
        if kind == "keyboard":
            # Key 0..127 straight onto the whole range: what a filter that tracks the
            # keyboard wants without any setting.
            fields.update({"low": 0, "high": 127, "center": 0.0, "depth": 1.0})
        if kind == "xy" and channels:
            fields["target_y"] = channel_path(str(channels[0]["name"]), "pan")
        return self.session.build_and_execute("AddController", fields)

    def remove(self) -> None:
        """Remove the selected controller."""
        controller = self.current()
        if controller is not None:
            self.session.build_and_execute("RemoveController", int(controller["id"]))


def _label(controller: dict[str, Any]) -> str:
    kind = str(controller["kind"])
    target = str(controller["target"])
    if kind == "xy":
        target = f"{target} / {controller['target_y']}"
    return f"{controller['name']}  ({KIND_LABELS.get(kind, kind)} → {target})"
