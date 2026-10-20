"""The generic parameter editor - Phase 6's key leverage point (phase_6.md 4.4).

Phase 4 gave every instrument and effect a ParamDescriptor table: name, range, default,
unit, scale. That is enough to generate a real editor, so thirteen instruments and
twenty-seven effects get ONE editor, built here, and hand-built editors are written only
where a grid of controls genuinely fails (the panels' ``editors/``).

The unit and scale choose the widget and its formatting (``generic_editor_units``):

    Hz, logarithmic   -> log knob,      "1.2 kHz"
    dB                -> fader,         "-6.0 dB"
    ms, s             -> numeric drag,  "250 ms"
    normalized, %     -> knob,          "45 %"
    stepped, count    -> combo
    bool              -> combo (Off / On)
    anything else     -> knob with its unit

Every control turns through :meth:`Session.set_param` - heard next block, recorded as a
command, coalesced so a drag is one undo step - and offers the same context menu:
automate (one command: a lane on this parameter, starting at its current value), MIDI
learn (Phase 9's; present and disabled), and reset.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from functools import partial
from typing import Any

from PySide6.QtCore import QPoint, Qt
from PySide6.QtWidgets import (
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QLabel,
    QMenu,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from adx.widgets.combo import ParamCombo, stepped_options
from adx.widgets.fader import ValueFader
from adx.widgets.knob import Knob
from adx.widgets.numeric_drag import NumericDrag

#: Names for the stepped parameters whose values are choices, not counts.
ENUMS: dict[str, list[str]] = {
    "type": ["Off", "Peak", "Low shelf", "High shelf", "Low cut", "High cut", "Notch", "Band pass"],
    "wave": ["Sine", "Triangle", "Saw", "Square", "Pulse", "Noise"],
    "detector": ["Peak", "RMS"],
    "mode": ["Soft", "Tube", "Hard"],
    "model": ["Kick", "Snare", "Hat", "Clap", "Tom", "GM kit"],
    "shape": ["Sine", "Triangle", "Saw", "Square", "Ramp", "Random"],
    "interpolation": ["Linear", "Cubic", "Sinc"],
    "carrier": ["Sidechain", "Saw", "Pulse", "Noise"],
}


def quote_segment(name: str) -> str:
    """A path segment, quoted when it must be (ParamRegistry::quoteSegment)."""
    if name and all(c.isascii() and (c.isalnum() or c in "_-") for c in name):
        return name
    escaped = name.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def channel_path(channel: str, parameter: str) -> str:
    """``channel.<name>.<parameter>``."""
    return f"channel.{quote_segment(channel)}.{parameter}"


def slot_path(insert: int, slot: int, parameter: str) -> str:
    """``insert.<n>.slot.<m>.<parameter>``."""
    return f"insert.{insert}.slot.{slot}.{parameter}"


def format_value(value: float, unit: str) -> str:
    """A value with its unit, the way the editor shows it."""
    if unit == "Hz":
        return f"{value / 1000.0:.3g} kHz" if abs(value) >= 1000.0 else f"{value:.3g} Hz"
    if unit == "dB":
        return f"{value:+.1f} dB" if value else "0.0 dB"
    if unit == "ms":
        return f"{value:.0f} ms" if abs(value) >= 10 else f"{value:.2g} ms"
    if unit == "s":
        return f"{value:.3g} s"
    if unit in ("normalized", "%"):
        scale = 1.0 if unit == "%" else 100.0
        return f"{value * scale:.0f} %"
    if unit == "st":
        return f"{value:+.2g} st"
    if unit == "cents":
        return f"{value:+.0f} ct"
    if unit == "ratio":
        return f"{value:.3g}x"
    if unit == "bool":
        return "On" if value >= 0.5 else "Off"
    return f"{value:.3g}"


#: The widget kind each (unit, scale) gets - the table generic_editor_units checks.
def widget_kind(descriptor: dict[str, Any]) -> str:
    """ "combo", "logknob", "fader", "drag" or "knob"."""
    unit, scale = str(descriptor["unit"]), str(descriptor["scale"])
    if scale == "stepped" or unit in ("count", "bool"):
        return "combo"
    if unit == "Hz" and scale == "log":
        return "logknob"
    if unit == "dB":
        return "fader"
    if unit in ("ms", "s"):
        return "drag"
    return "knob"


def _none() -> dict[str, str]:
    return {}


@dataclass(slots=True)
class ParamTarget:
    """Where an editor's parameters live and how it changes them. Reads are batched:
    one refresh is one read of every value, never one read per control."""

    #: The parameter's path, from its name.
    path_of: Callable[[str], str]
    #: Every parameter's current value by name, read now (absent: the default).
    read_values: Callable[[], dict[str, float]]
    #: A knob turn.
    turn: Callable[[str, float], None]
    #: "Create automation" on a parameter, by name; None disables the entry.
    automate: Callable[[str], None] | None = None
    #: Every automated path, read now, with its lane's colour.
    automated_paths: Callable[[], dict[str, str]] = _none


def make_control(descriptor: dict[str, Any], value: float) -> QWidget:
    """The widget for one descriptor, showing ``value``."""
    name = str(descriptor["name"])
    leaf = name.rsplit(".", 1)[-1]
    unit = str(descriptor["unit"])
    low, high = float(descriptor["min"]), float(descriptor["max"])
    default = float(descriptor["default"])
    kind = widget_kind(descriptor)
    formatter: Callable[[float], str] = partial(format_value, unit=unit)
    if kind == "combo":
        names = ["Off", "On"] if unit == "bool" else ENUMS.get(leaf)
        return ParamCombo(stepped_options(low, high, names), value)
    if kind == "fader":
        return ValueFader(low, high, value, default, label=leaf, formatter=formatter)
    if kind == "drag":
        return NumericDrag(
            low,
            high,
            value,
            default,
            label=leaf,
            formatter=formatter,
            logarithmic=str(descriptor["scale"]) == "log",
        )
    return Knob(
        low, high, value, default, label=leaf, logarithmic=kind == "logknob", formatter=formatter
    )


def group_of(name: str) -> str:
    """The section a parameter belongs to: its prefix ("env", "op1"), or "Main"."""
    return name.rsplit(".", 1)[0] if "." in name else "Main"


class GenericEditor(QScrollArea):
    """Every value parameter of a type, grouped into sections by prefix."""

    def __init__(
        self,
        descriptors: list[dict[str, Any]],
        target: ParamTarget,
        parent: QWidget | None = None,
        columns: int = 6,
    ) -> None:
        super().__init__(parent)
        self.setWidgetResizable(True)
        self.target = target
        self.controls: dict[str, QWidget] = {}
        self.descriptors = {str(d["name"]): d for d in descriptors if int(d["curve_part"]) == 0}
        body = QWidget()
        outer = QVBoxLayout(body)
        sections: dict[str, QGridLayout] = {}
        counts: dict[str, int] = {}
        values = target.read_values()
        for name, descriptor in self.descriptors.items():
            group = group_of(name)
            if group not in sections:
                box = QGroupBox(group)
                sections[group] = QGridLayout(box)
                counts[group] = 0
                outer.addWidget(box)
            value = values.get(name, float(descriptor["default"]))
            control = make_control(descriptor, value)
            control.setObjectName(f"param:{name}")
            self._wire(name, control, descriptor)
            index = counts[group]
            sections[group].addWidget(control, index // columns, index % columns)
            counts[group] = index + 1
            self.controls[name] = control
        if not self.descriptors:
            outer.addWidget(QLabel("This type has no parameters."))
        outer.addStretch(1)
        self.setWidget(body)
        self.refresh()

    def _wire(self, name: str, control: QWidget, descriptor: dict[str, Any]) -> None:
        path = self.target.path_of(name)
        changed = getattr(control, "value_changed", None)
        if changed is not None:
            changed.connect(lambda value, n=name: self.target.turn(n, float(value)))
        requested = getattr(control, "context_requested", None)
        if requested is not None:
            requested.connect(lambda point, n=name, d=descriptor: self._context_menu(n, d, point))
        if str(descriptor.get("rate", "")) == "baked":
            control.setToolTip(control.toolTip() + " - re-renders the voice when changed")
        control.setProperty("paramPath", path)

    def _context_menu(self, name: str, descriptor: dict[str, Any], point: QPoint) -> None:
        menu = self.context_menu(name, descriptor)
        menu.exec(point)

    def context_menu(self, name: str, descriptor: dict[str, Any]) -> QMenu:
        """Automate, MIDI learn (Phase 9), reset - every parameter's menu."""
        menu = QMenu(self)
        automate = menu.addAction("Create automation")
        automate.setEnabled(self.target.automate is not None)
        if self.target.automate is not None:
            callback = self.target.automate
            automate.triggered.connect(lambda: callback(name))
        learn = menu.addAction("MIDI learn")
        learn.setEnabled(False)
        learn.setToolTip("MIDI learn arrives with MIDI input in Phase 9 (plans/phase_9.md)")
        reset = menu.addAction("Reset to default")
        default = float(descriptor["default"])
        reset.triggered.connect(lambda: self.target.turn(name, default))
        return menu

    def refresh(self) -> None:
        """Show the model's current values (after an undo, an automation write...): one
        read of the values, one of the automated paths."""
        values = self.target.read_values()
        automated = self.target.automated_paths()
        for name, control in self.controls.items():
            value = values.get(name, float(self.descriptors[name]["default"]))
            setter = getattr(control, "set_value", None)
            if setter is not None and getattr(control, "value", None) != value:
                setter(value)
            if isinstance(control, Knob):
                color = automated.get(self.target.path_of(name))
                if color != control.automation_color:
                    control.automation_color = color
                    control.update()


def form_row(layout: QFormLayout, label: str, widget: QWidget) -> None:
    """A labelled row, right-aligned label."""
    caption = QLabel(label)
    caption.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
    layout.addRow(caption, widget)
