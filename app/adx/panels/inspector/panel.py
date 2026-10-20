"""The inspector (phase_6.md 4.7): properties of whatever is selected, of any kind.

One place to see and type exact values: a note's start, length, pitch and velocity; a
clip's start, length and offset; a channel's volume and pan; an insert's gain; a slot's
effect and mix; the selected automation points' values. Every field it edits is a
command through the session, so the inspector is a panel like any other.
"""

from __future__ import annotations

from collections.abc import Callable
from functools import partial

import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QDoubleSpinBox, QFormLayout, QLabel, QWidget

from adx import engine_bridge
from adx.panels.base import DockPanel
from adx.panels.params import channel_path
from adx.selection import Kind, Selection
from adx.session import Session


class InspectorPanel(DockPanel):
    """The selection's properties."""

    panel_id = "inspector"
    title = "Inspector"
    default_area = Qt.DockWidgetArea.RightDockWidgetArea

    def __init__(
        self, session: Session, selection: Selection, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self.session = session
        self.selection = selection
        body = QWidget()
        self.form = QFormLayout(body)
        self.setWidget(body)
        session.changed.connect(self.refresh)
        selection.changed.connect(self.refresh)

    def _clear(self) -> None:
        while self.form.rowCount():
            self.form.removeRow(0)

    def _number(
        self,
        label: str,
        value: float,
        low: float,
        high: float,
        apply: Callable[[float], object],
        decimals: int = 0,
    ) -> None:
        spin = QDoubleSpinBox()
        spin.setRange(low, high)
        spin.setDecimals(decimals)
        spin.setValue(value)
        spin.setKeyboardTracking(False)
        spin.valueChanged.connect(lambda v: apply(float(v)))
        self.form.addRow(label, spin)

    def refresh(self) -> None:
        """Rebuild the form for the current selection from a fresh read."""
        self._clear()
        current = self.selection.current
        if self.session.project is None or not current:
            self.form.addRow(QLabel("Nothing selected"))
            return
        self.form.addRow(QLabel(f"{len(current.ids)} {current.kind.value} selected"))
        handler = {
            Kind.NOTES: self._notes,
            Kind.CLIPS: self._clips,
            Kind.CHANNELS: self._channels,
            Kind.INSERTS: self._inserts,
            Kind.SLOTS: self._slots,
            Kind.POINTS: self._points,
        }.get(current.kind)
        if handler is not None:
            handler()

    def _notes(self) -> None:
        context = self.selection.current.context
        notes = self.session.require().notes(context.pattern, context.channel)
        chosen = notes[
            np.isin(notes["id"], np.asarray(self.selection.current.ids, dtype=np.uint32))
        ]
        if len(chosen) == 0:
            return
        first = chosen[0]

        def update(field: str, value: float) -> None:
            rows = chosen.copy()
            rows[field] = value
            command = engine_bridge.commands.edit_notes(
                context.pattern,
                context.channel,
                np.zeros(0, dtype=np.uint32),
                rows,
                engine_bridge.empty_notes(),
                f"Set {field}",
            )
            self.session.execute(command)

        self._number("Start (ticks)", float(first["start"]), 0, 1e9, lambda v: update("start", v))
        self._number(
            "Length (ticks)", float(first["length"]), 1, 1e9, lambda v: update("length", v)
        )
        self._number("Pitch", float(first["pitch"]), 0, 127, lambda v: update("pitch", v))
        self._number("Velocity", float(first["velocity"]), 1, 127, lambda v: update("velocity", v))

    def _clips(self) -> None:
        items = self.session.require().playlist_items()
        rows = items[
            np.isin(items["item"], np.asarray(self.selection.current.ids, dtype=np.uint32))
        ]
        if len(rows) == 0:
            return
        row = rows[0]
        track, item = int(row["track"]), int(row["item"])

        def set_field(field: str, value: float) -> None:
            self.session.build_and_execute("SetPlaylistItem", track, item, {field: value})

        self._number("Start (ticks)", float(row["start"]), 0, 1e10, lambda v: set_field("start", v))
        self._number(
            "Length (ticks)", float(row["length"]), 1, 1e10, lambda v: set_field("length", v)
        )
        self._number(
            "Offset (ticks)", float(row["offset"]), 0, 1e10, lambda v: set_field("offset", v)
        )
        if int(row["kind"]) == 1:
            self._number(
                "Stretch", float(row["stretch"]), 0.05, 20, lambda v: set_field("stretch", v), 3
            )
            self._number(
                "Pitch (st)", float(row["pitch"]), -48, 48, lambda v: set_field("pitch", v), 2
            )

    def _channels(self) -> None:
        channel = self.selection.current.ids[0]
        info = next(
            (c for c in self.session.require().channels_info() if int(c["id"]) == channel), None
        )
        if info is None:
            return
        name = str(info["name"])
        self.form.addRow("Instrument", QLabel(str(info["instrument"])))
        self._number(
            "Volume",
            float(info["volume"]),
            0,
            2,
            lambda v: self.session.set_param(channel_path(name, "volume"), v),
            3,
        )
        self._number(
            "Pan",
            float(info["pan"]),
            -1,
            1,
            lambda v: self.session.set_param(channel_path(name, "pan"), v),
            3,
        )
        self._number(
            "Pitch (cents)",
            float(info["pitch"]),
            -2400,
            2400,
            lambda v: self.session.set_param(channel_path(name, "pitch"), v),
            0,
        )
        self._number(
            "Polyphony",
            float(info["polyphony"]),
            1,
            256,
            lambda v: self.session.build_and_execute("SetChannelField", channel, "polyphony", v),
        )

    def _inserts(self) -> None:
        insert = self.selection.current.ids[0]
        info = next(
            (i for i in self.session.require().mixer_info()["inserts"] if int(i["id"]) == insert),
            None,
        )
        if info is None:
            return
        self.form.addRow("Name", QLabel(str(info["name"])))
        for field in ("gain", "pan", "width"):
            low, high = (-1.0, 1.0) if field == "pan" else (0.0, 2.0)
            self._number(
                field.title(),
                float(info[field]),
                low,
                high,
                partial(self._set_path, f"insert.{insert}.{field}"),
                3,
            )

    def _slots(self) -> None:
        slot = self.selection.current.ids[0]
        insert = self.selection.current.context.insert
        for entry in self.session.require().mixer_info()["inserts"]:
            for info in entry["slots"]:
                if int(info["id"]) == slot:
                    self.form.addRow("Effect", QLabel(str(info["type"])))
                    owner = int(entry["id"]) or insert
                    self._number(
                        "Mix",
                        float(info["mix"]),
                        0,
                        1,
                        partial(self._set_path, f"insert.{owner}.slot.{slot}.mix"),
                        3,
                    )

    def _set_path(self, path: str, value: float) -> None:
        """A spin box turned a parameter."""
        self.session.set_param(path, value)

    def _points(self) -> None:
        clip = self.selection.current.context.clip
        points = self.session.require().automation_points(clip)
        index = self.selection.current.ids[0]
        if not 0 <= index < len(points):
            return

        def set_value(value: float) -> None:
            updated = points.copy()
            updated[index, 1] = value
            self.session.build_and_execute("SetBreakpoints", clip, updated)

        self._number("Value", float(points[index, 1]), -1e6, 1e6, set_value, 4)
