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
learn (Phase 9: the next control that moves is bound to it), and reset.
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
    "model": [
        "Kick",
        "Snare",
        "Hat",
        "Clap",
        "Tom",
        "GM kit",
        "Cowbell",
        "Cymbal",
        "Rim",
        "Clave",
        "Shaker",
        "Conga",
    ],
    "character": ["808", "909", "707"],
    "shape": ["Sine", "Triangle", "Saw", "Square", "Ramp", "Random"],
    "interpolation": ["Linear", "Cubic", "Sinc"],
    "carrier": ["Sidechain", "Saw", "Pulse", "Noise"],
}

_SYNC = ["Free", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 bar"]

#: Names for one type's choices where its leaf name means something else elsewhere
#: ("mode" is the Clipper's curve but the Ensemble's voicing). Looked up first.
TYPE_ENUMS: dict[str, list[str]] = {
    "Clipper.mode": ["Soft", "Hard", "Sine fold", "Foldback", "Asym tube"],
    "Filter.type": [
        "Ladder",
        "Low pass",
        "Band pass",
        "High pass",
        "Notch",
        "Comb +",
        "Comb -",
        "Formant",
    ],
    "Filter.sync": _SYNC,
    "Filter.shape": ["Sine", "Triangle", "Saw down", "Square"],
    "VolumeShaper.length": ["1/16", "1/8", "1/4", "1/2", "1 bar"],
    "VolumeShaper.shape": ["Pump", "Ramp up", "Ramp down", "Sine"],
    "Tape.speed": ["15 ips", "7.5 ips"],
    "Ensemble.mode": ["Juno I", "Juno II", "Juno I+II", "BBD", "Dimension"],
    "Rotary.speed": ["Slow", "Fast"],
    "Hall.shimmerInterval": ["+12", "+7", "+19"],
    "PitchCorrect.key": ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"],
    "PitchCorrect.scale": [
        "Chromatic",
        "Major",
        "Minor",
        "Harmonic minor",
        "Major pentatonic",
        "Minor pentatonic",
        "Blues",
        "Dorian",
    ],
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


#: Arms MIDI learn on a parameter path: the main window sets it (PluginRuntime.arm), so
#: an editor built anywhere offers learn without being handed the runtime.
LEARN_HOOK: list[Callable[[str], None]] = []


def set_learn_hook(hook: Callable[[str], None] | None) -> None:
    """Install (or, with None, remove) what "MIDI learn" calls."""
    LEARN_HOOK.clear()
    if hook is not None:
        LEARN_HOOK.append(hook)


def arm_learn(path: str) -> bool:
    """Arm learn on ``path``; False when nothing listens (no MIDI, a test)."""
    if not LEARN_HOOK:
        return False
    LEARN_HOOK[0](path)
    return True


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
    #: "MIDI learn" on a parameter, by name: makes it addressable, then arms learn.
    learn: Callable[[str], None] | None = None
    #: Every automated path, read now, with its lane's colour.
    automated_paths: Callable[[], dict[str, str]] = _none


def make_control(descriptor: dict[str, Any], value: float, type_name: str = "") -> QWidget:
    """The widget for one descriptor of type ``type_name``, showing ``value``."""
    name = str(descriptor["name"])
    leaf = name.rsplit(".", 1)[-1]
    unit = str(descriptor["unit"])
    low, high = float(descriptor["min"]), float(descriptor["max"])
    default = float(descriptor["default"])
    kind = widget_kind(descriptor)
    formatter: Callable[[float], str] = partial(format_value, unit=unit)
    if kind == "combo":
        names = (
            ["Off", "On"]
            if unit == "bool"
            else TYPE_ENUMS.get(f"{type_name}.{leaf}", ENUMS.get(leaf))
        )
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
        type_name: str = "",
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
            control = make_control(descriptor, value, type_name)
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
        control.setObjectName(f"param:{path}")

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
        learn.setEnabled(self.target.learn is not None and bool(LEARN_HOOK))
        if self.target.learn is not None:
            learner = self.target.learn
            learn.triggered.connect(lambda: learner(name))
        learn.setToolTip("Move a control on a MIDI input to bind it to this parameter")
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
