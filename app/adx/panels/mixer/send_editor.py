"""Sends: the menu a strip's "+ Send" opens, and the editor for one send (phase_6.md 4.3).

A send's targets are the inserts the routing matrix says it may feed: never itself,
never one that would close a cycle - the same engine answer the matrix draws.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Signal
from PySide6.QtWidgets import QCheckBox, QHBoxLayout, QLabel, QMenu, QWidget

from adx.engine_bridge import Project
from adx.panels.mixer.routing_matrix import CYCLE, SELF, SEND
from adx.widgets.knob import Knob


def send_targets(project: Project, insert: int) -> list[tuple[int, str]]:
    """(id, name) of every insert ``insert`` may send to."""
    ids, states = project.routing_matrix()
    if insert not in ids:
        return []
    row = ids.index(insert)
    names = {int(i["id"]): str(i["name"]) for i in project.mixer_info()["inserts"]}
    return [
        (target, names.get(target, ""))
        for column, target in enumerate(ids)
        if int(states[row, column]) not in (CYCLE, SELF, SEND)
    ]


def send_menu(project: Project, insert: int, parent: QWidget | None = None) -> QMenu:
    """A menu of valid targets; each action's data is the target id."""
    menu = QMenu(parent)
    for target, name in send_targets(project, insert):
        action = menu.addAction(f"{target} {name}".strip())
        action.setData(target)
    if menu.isEmpty():
        menu.addAction("(no insert can take a send from here)").setEnabled(False)
    return menu


class SendEditor(QWidget):
    """One send: level and pre/post. Emits ``level_turned`` and ``pre_set``."""

    level_turned = Signal(int, float)
    pre_set = Signal(int, bool)

    def __init__(
        self, send: dict[str, Any], target_name: str, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.send = int(send["id"])
        layout = QHBoxLayout(self)
        layout.addWidget(QLabel(f"→ {target_name}"))
        self.level = Knob(0.0, 1.0, float(send["level"]), 0.5, "level")
        self.level.value_changed.connect(lambda v: self.level_turned.emit(self.send, v))
        layout.addWidget(self.level)
        self.pre = QCheckBox("pre-fader")
        self.pre.setChecked(bool(send["pre"]))
        self.pre.toggled.connect(lambda on: self.pre_set.emit(self.send, on))
        layout.addWidget(self.pre)
