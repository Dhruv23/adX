"""The rack view (phase_12.md 4.1, 4.2): each inserted rack's macros as knobs.

A rack names its macros `<rack>/<macro>`, so they group by rack here; a macro the user
made by hand is its own group. Turning a knob is `set_macro`, one coalesced undo step per
gesture, heard at the next snapshot like any parameter. Each knob's tooltip lists what it
moves and between which values.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QDial,
    QGridLayout,
    QGroupBox,
    QLabel,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from adx.panels.base import DockPanel
from adx.session import Session

#: A knob's steps: 0..1 in thousandths.
STEPS = 1000
#: Knobs per row: eight is a rack's maximum (taxonomy.MACRO_SLOTS).
PER_ROW = 8


def group_of(name: str) -> tuple[str, str]:
    """(rack, macro) for `rack/macro`; ("Macros", name) for a macro of no rack."""
    rack, slash, macro = name.rpartition("/")
    return (rack, macro) if slash else ("Macros", name)


class RackPanel(DockPanel):
    """Macro knobs, by rack."""

    panel_id = "rack"
    title = "Macros"
    default_area = Qt.DockWidgetArea.RightDockWidgetArea

    def __init__(self, session: Session, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.session = session
        self.knobs: dict[int, QDial] = {}
        self._signature: tuple[tuple[int, str, int], ...] = ()
        self.knob_area = QWidget()
        self.layout_ = QVBoxLayout(self.knob_area)
        self.layout_.setAlignment(Qt.AlignmentFlag.AlignTop)
        self.empty = QLabel("No macros. Drop a rack from the browser's Sounds onto the rack.")
        self.empty.setWordWrap(True)
        self.layout_.addWidget(self.empty)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self.knob_area)
        self.setWidget(scroll)
        session.changed.connect(self.refresh)
        self.refresh()

    def macros(self) -> list[dict[str, Any]]:
        project = self.session.project
        return project.macros() if project is not None else []

    def refresh(self) -> None:
        """Rebuild the knobs when the set of macros changed; else just move them."""
        macros = self.macros()
        signature = tuple((int(m["id"]), str(m["name"]), len(m["targets"])) for m in macros)
        if signature != self._signature:
            self._rebuild(macros)
            self._signature = signature
        for macro in macros:
            knob = self.knobs.get(int(macro["id"]))
            if knob is not None and not knob.isSliderDown():
                knob.blockSignals(True)
                knob.setValue(round(float(macro["value"]) * STEPS))
                knob.blockSignals(False)

    def _rebuild(self, macros: list[dict[str, Any]]) -> None:
        while self.layout_.count() > 1:
            item = self.layout_.takeAt(1)
            widget = item.widget() if item is not None else None
            if widget is not None:
                widget.deleteLater()
        self.knobs.clear()
        self.empty.setVisible(not macros)
        groups: dict[str, list[dict[str, Any]]] = {}
        for macro in macros:
            groups.setdefault(group_of(str(macro["name"]))[0], []).append(macro)
        for rack, members in groups.items():
            box = QGroupBox(rack)
            grid = QGridLayout(box)
            for i, macro in enumerate(members):
                knob = QDial()
                knob.setRange(0, STEPS)
                knob.setNotchesVisible(True)
                knob.setFixedSize(48, 48)
                knob.setObjectName(f"macro.{macro['name']}")
                knob.setToolTip(
                    "\n".join(
                        f"{t['path']}: {t['min']:g} .. {t['max']:g}"
                        + ("" if t["resolved"] else "  (does not resolve)")
                        for t in macro["targets"]
                    )
                )
                macro_id = int(macro["id"])
                knob.valueChanged.connect(lambda v, m=macro_id: self.turn(m, v / STEPS))
                knob.sliderPressed.connect(self.session.seal)
                label = QLabel(group_of(str(macro["name"]))[1])
                label.setAlignment(Qt.AlignmentFlag.AlignHCenter)
                row, column = divmod(i, PER_ROW)
                grid.addWidget(knob, row * 2, column, Qt.AlignmentFlag.AlignHCenter)
                grid.addWidget(label, (row * 2) + 1, column)
                self.knobs[macro_id] = knob
            self.layout_.addWidget(box)

    def turn(self, macro: int, value: float) -> None:
        """A knob turn: the macro to `value` (0..1)."""
        project = self.session.project
        if project is not None:
            self.session.call(project.set_macro, macro, value)
