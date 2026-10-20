"""VA: the filter on an XY pad (cutoff across, resonance up) above the amp and filter
envelopes. Everything else is the generic grid."""

from __future__ import annotations

from typing import Any

from PySide6.QtWidgets import QHBoxLayout, QLabel, QVBoxLayout, QWidget

from adx.panels.editor_parts import TabbedEditor
from adx.panels.instrument.editors.common import tabbed
from adx.panels.instrument.generic_editor import channel_target, descriptors_of
from adx.panels.params import ParamTarget
from adx.session import Session
from adx.widgets.xy_pad import XYPad


class FilterPad(QWidget):
    """Two parameters on one pad: x and y normalised over their descriptors' ranges."""

    def __init__(self, target: ParamTarget, instrument: str, x_name: str, y_name: str) -> None:
        super().__init__()
        self.target = target
        descriptors = {
            str(d["name"]): d for d in descriptors_of(instrument) if int(d["curve_part"]) == 0
        }
        self.x_descriptor = descriptors.get(x_name)
        self.y_descriptor = descriptors.get(y_name)
        self.x_name, self.y_name = x_name, y_name
        layout = QHBoxLayout(self)
        column = QVBoxLayout()
        column.addWidget(QLabel(f"{x_name} →\n{y_name} ↑"))
        column.addStretch(1)
        layout.addLayout(column)
        self.pad = XYPad()
        self.pad.moved.connect(self._moved)
        layout.addWidget(self.pad, 1)
        self.refresh()

    @staticmethod
    def _to(descriptor: dict[str, Any] | None, position: float) -> float:
        if descriptor is None:
            return position
        low, high = float(descriptor["min"]), float(descriptor["max"])
        if str(descriptor["scale"]) == "log" and low > 0:
            return float(low * (high / low) ** position)
        return low + position * (high - low)

    @staticmethod
    def _from(descriptor: dict[str, Any] | None, value: float) -> float:
        if descriptor is None:
            return value
        low, high = float(descriptor["min"]), float(descriptor["max"])
        if str(descriptor["scale"]) == "log" and low > 0 and value > 0:
            import math

            return math.log(value / low) / math.log(high / low)
        return (value - low) / (high - low) if high > low else 0.0

    def refresh(self) -> None:
        """One read."""
        values = self.target.read_values()
        x = values.get(
            self.x_name, float(self.x_descriptor["default"]) if self.x_descriptor else 0.5
        )
        y = values.get(
            self.y_name, float(self.y_descriptor["default"]) if self.y_descriptor else 0.5
        )
        self.pad.set_position(self._from(self.x_descriptor, x), self._from(self.y_descriptor, y))

    def _moved(self, x: float, y: float) -> None:
        self.target.turn(self.x_name, self._to(self.x_descriptor, x))
        self.target.turn(self.y_name, self._to(self.y_descriptor, y))


def va_editor(session: Session, channel: int) -> TabbedEditor:
    """Filter pad and envelopes, beside every parameter."""
    pad = FilterPad(channel_target(session, channel), "va", "filter.cutoff", "filter.resonance")
    return tabbed(session, channel, "va", pad)
