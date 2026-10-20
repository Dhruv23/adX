"""The generic parameter editor (phase_6.md 4.4, 5): ``generic_editor_covers_all`` and
``generic_editor_units``. Every instrument and effect from Phase 4 is editable - the
phase's definition-of-done line - is the first test."""

from __future__ import annotations

from typing import Any

import pytest
from PySide6.QtWidgets import QWidget

from adx import engine_bridge
from adx.mainwindow import MainWindow
from adx.panels.params import format_value, make_control, widget_kind
from adx.widgets.combo import ParamCombo
from adx.widgets.fader import ValueFader
from adx.widgets.knob import Knob
from adx.widgets.numeric_drag import NumericDrag


def catalog(kind: str) -> list[dict[str, Any]]:
    return [t for t in engine_bridge.type_catalog() if t["kind"] == kind]


def value_params(entry: dict[str, Any]) -> set[str]:
    """The parameters an editor must show: every value parameter (curve parts are edited
    through their parent's curve widget)."""
    return {str(p["name"]) for p in entry["params"] if int(p["curve_part"]) == 0}


def shown(panel: QWidget) -> set[str]:
    return {
        w.objectName().removeprefix("param:")
        for w in panel.findChildren(QWidget)
        if w.objectName().startswith("param:")
    }


def test_generic_editor_covers_all_instruments(studio: MainWindow) -> None:
    session = studio.session
    for entry in catalog("instrument"):
        name = f"I_{entry['name']}"
        assert session.build_and_execute("AddChannelNew", name, str(entry["name"]), 0, "")
        channel = int(studio.project.channels_info()[-1]["id"])
        studio.instrument.show_channel(channel)
        wanted = value_params(entry)
        got = shown(studio.instrument)
        assert got or not wanted, f"{entry['name']}: empty editor"
        assert wanted <= got, f"{entry['name']}: missing {sorted(wanted - got)}"


def test_generic_editor_covers_all_effects(studio: MainWindow) -> None:
    session = studio.session
    master = int(studio.project.mixer_info()["master"])
    for entry in catalog("effect"):
        assert session.build_and_execute("AddSlot", master, str(entry["name"]))
        slot = int(studio.project.mixer_info()["inserts"][0]["slots"][-1]["id"])
        studio.effect.show_slot(master, slot)
        wanted = value_params(entry)
        got = shown(studio.effect)
        assert got or not wanted, f"{entry['name']}: empty editor"
        assert wanted <= got, f"{entry['name']}: missing {sorted(wanted - got)}"


def test_a_control_turns_its_parameter(studio: MainWindow) -> None:
    master = int(studio.project.mixer_info()["master"])
    studio.session.build_and_execute("AddSlot", master, "Delay")
    slot = int(studio.project.mixer_info()["inserts"][0]["slots"][-1]["id"])
    studio.effect.show_slot(master, slot)
    controls = {w.objectName(): w for w in studio.effect.findChildren(QWidget) if w.objectName()}
    feedback = controls["param:feedback"]
    path = str(feedback.property("paramPath"))
    assert path == f"insert.{master}.slot.{slot}.feedback"
    depth = studio.project.undo_depth()
    for value in (0.2, 0.3, 0.4):
        feedback.value_changed.emit(value)
    assert abs(studio.project.param_value(path) - 0.4) < 1e-6
    assert studio.project.undo_depth() == depth + 1  # coalesced: one step


UNITS = [
    # (unit, scale, widget class, kind, value, text)
    ("Hz", "log", Knob, "logknob", 1200.0, "1.2 kHz"),
    ("Hz", "linear", Knob, "knob", 440.0, "440 Hz"),
    ("dB", "linear", ValueFader, "fader", -6.0, "-6.0 dB"),
    ("ms", "linear", NumericDrag, "drag", 250.0, "250 ms"),
    ("s", "log", NumericDrag, "drag", 1.5, "1.5 s"),
    ("normalized", "linear", Knob, "knob", 0.45, "45 %"),
    ("%", "linear", Knob, "knob", 45.0, "45 %"),
    ("count", "stepped", ParamCombo, "combo", 3.0, "3"),
    ("bool", "stepped", ParamCombo, "combo", 1.0, "On"),
    ("st", "linear", Knob, "knob", 7.0, "+7 st"),
    ("cents", "linear", Knob, "knob", -25.0, "-25 ct"),
    ("ratio", "log", Knob, "knob", 4.0, "4x"),
]


@pytest.mark.parametrize(("unit", "scale", "cls", "kind", "value", "text"), UNITS)
def test_generic_editor_units(
    qapp: object, unit: str, scale: str, cls: type, kind: str, value: float, text: str
) -> None:
    descriptor = {
        "name": "x",
        "unit": unit,
        "scale": scale,
        "min": 0.0 if unit not in ("dB", "st", "cents") else -60.0,
        "max": 20000.0,
        "default": 0.0 if unit != "Hz" else 1000.0,
        "curve_part": 0,
    }
    if unit == "Hz" and scale == "log":
        descriptor["min"] = 20.0
    if unit in ("count", "bool"):
        descriptor["min"], descriptor["max"] = 0.0, 8.0 if unit == "count" else 1.0
    assert widget_kind(descriptor) == kind
    widget = make_control(descriptor, value)
    assert isinstance(widget, cls)
    if unit not in ("count", "bool"):
        assert format_value(value, unit) == text
    else:
        assert isinstance(widget, ParamCombo)
        assert widget.currentText() == text
    widget.deleteLater()
