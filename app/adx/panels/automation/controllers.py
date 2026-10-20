"""Controllers: automation sources rather than drawn curves (phase_6.md 4.5).

An LFO, an envelope (retriggered by a channel's notes) or a peak follower (an insert's
level) drives one parameter. They are engine-side - the LFO and envelope compile into
automation lanes, the peak follower is a graph node - so this is only their settings:
add, edit, remove, each one command.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QStringListModel, Signal
from PySide6.QtWidgets import (
    QComboBox,
    QCompleter,
    QFormLayout,
    QHBoxLayout,
    QLineEdit,
    QListWidget,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from adx import engine_bridge
from adx.engine_bridge import PPQ
from adx.panels.params import channel_path, slot_path
from adx.session import Session
from adx.widgets.knob import Knob
from adx.widgets.numeric_drag import NumericDrag

SHAPES = ("sine", "triangle", "saw", "square")
PERIODS = (
    ("1/16", PPQ // 4),
    ("1/8", PPQ // 2),
    ("1/4", PPQ),
    ("1/2", PPQ * 2),
    ("1 bar", PPQ * 4),
    ("2 bars", PPQ * 8),
    ("4 bars", PPQ * 16),
)


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
    return paths


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
        buttons = QHBoxLayout()
        for kind in ("lfo", "envelope", "peak"):
            button = QPushButton(f"+ {kind}")
            button.clicked.connect(lambda _c=False, k=kind: self.add(k))
            buttons.addWidget(button)
        remove = QPushButton("Remove")
        remove.clicked.connect(self.remove)
        buttons.addWidget(remove)
        left.addLayout(buttons)
        layout.addLayout(left, 1)
        self.form_host = QWidget()
        self.form = QFormLayout(self.form_host)
        layout.addWidget(self.form_host, 2)
        self._ids: list[int] = []

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
        if ids != self._ids:
            row = self.list.currentRow()
            self.list.blockSignals(True)
            self.list.clear()
            for c in controllers:
                self.list.addItem(f"{c['name']}  ({c['kind']} → {c['target']})")
            self.list.blockSignals(False)
            self._ids = ids
            self.list.setCurrentRow(min(max(row, 0), len(ids) - 1))
        self._build_form(self.current())

    def _build_form(self, controller: dict[str, Any] | None) -> None:
        while self.form.rowCount():
            self.form.removeRow(0)
        if controller is None:
            return
        cid = int(controller["id"])
        target = QLineEdit(str(controller["target"]))
        target.setCompleter(QCompleter(QStringListModel(parameter_paths(self.session), target)))
        target.editingFinished.connect(lambda: self.set(cid, {"target": target.text().strip()}))
        self.form.addRow("Target", target)
        kind = str(controller["kind"])
        if kind == "lfo":
            shape = QComboBox()
            shape.addItems(list(SHAPES))
            shape.setCurrentText(str(controller["shape"]))
            shape.activated.connect(lambda i: self.set(cid, {"shape": SHAPES[i]}))
            self.form.addRow("Shape", shape)
            period = QComboBox()
            period.addItems([label for label, _t in PERIODS])
            ticks = [t for _l, t in PERIODS]
            current = int(controller["period"])
            period.setCurrentIndex(ticks.index(current) if current in ticks else 2)
            period.activated.connect(lambda i: self.set(cid, {"period": ticks[i]}))
            self.form.addRow("Period", period)
            self._knob(cid, "phase", controller, 0.0, 1.0)
        else:
            source = QComboBox()
            project = self.session.require()
            if kind == "envelope":
                choices = [(int(c["id"]), str(c["name"])) for c in project.channels_info()]
                key, current_source = "source_channel", int(controller["source_channel"])
            else:
                choices = [(int(i["id"]), str(i["name"])) for i in project.mixer_info()["inserts"]]
                key, current_source = "source_insert", int(controller["source_insert"])
            for _i, label in choices:
                source.addItem(label)
            ids = [i for i, _l in choices]
            if current_source in ids:
                source.setCurrentIndex(ids.index(current_source))
            source.activated.connect(lambda i: self.set(cid, {key: ids[i]}))
            self.form.addRow("Source", source)
            for field, high in (("attack", 2000.0), ("decay", 5000.0), ("release", 5000.0)):
                if kind == "peak" and field == "decay":
                    continue
                drag = NumericDrag(
                    0.0,
                    high,
                    float(controller[field]),
                    label=field,
                    formatter=lambda v: f"{v:.0f} ms",
                )
                drag.edit_finished.connect(lambda v, f=field: self.set(cid, {f: float(v)}))
                self.form.addRow(field.title(), drag)
            if kind == "envelope":
                self._knob(cid, "sustain", controller, 0.0, 1.0)
            else:
                self._knob(cid, "gain", controller, -24.0, 24.0)
        self._knob(cid, "depth", controller, -1.0, 1.0)
        self._knob(cid, "center", controller, 0.0, 1.0)

    def _knob(
        self, cid: int, field: str, controller: dict[str, Any], low: float, high: float
    ) -> None:
        knob = Knob(low, high, float(controller[field]), label=field)
        knob.edit_finished.connect(lambda v: self.set(cid, {field: float(v)}))
        self.form.addRow(field.title(), knob)

    # --- edits ----------------------------------------------------------------------

    def set(self, controller: int, fields: dict[str, Any]) -> None:
        """One SetController: consecutive edits of one controller coalesce."""
        if "target" in fields and not self.session.require().param_valid(str(fields["target"])):
            self.session.failed.emit("controller", f"{fields['target']} does not name a parameter")
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
        n = 1
        while f"{kind.title()}{n}" in taken:
            n += 1
        fields: dict[str, Any] = {"name": f"{kind.title()}{n}", "kind": kind, "target": target}
        if kind == "envelope" and channels:
            fields["source_channel"] = int(channels[0]["id"])
        if kind == "peak":
            fields["source_insert"] = int(project.mixer_info()["master"])
        return self.session.build_and_execute("AddController", fields)

    def remove(self) -> None:
        """Remove the selected controller."""
        controller = self.current()
        if controller is not None:
            self.session.build_and_execute("RemoveController", int(controller["id"]))
