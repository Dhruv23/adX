"""MIDI learn from the generic editor (phase_9.md 6): the Phase 6 menu entry, enabled
now, on a native effect's parameter and on a hosted plugin's - and the binding it makes
once a control moves."""

from __future__ import annotations

import pathlib
from typing import Any

import pytest
from PySide6.QtGui import QAction

from adx.engine_bridge import Plugins
from adx.mainwindow import MainWindow
from adx.panels.effect.generic_editor import generic_for_slot
from adx.panels.params import GenericEditor

ROOT = pathlib.Path(__file__).resolve().parents[3]


def learn_action(editor: GenericEditor, name: str) -> QAction:
    menu = editor.context_menu(name, editor.descriptors[name])
    return next(a for a in menu.actions() if a.text() == "MIDI learn")


def bind_next_control(studio: MainWindow, control: dict[str, Any]) -> None:
    """What the engine would report once a control moves: fed to the runtime's tick."""
    reports = [control]
    studio.plugin_runtime.engine = type(
        "Moved",
        (),
        {
            "midi_last_control": lambda _self: reports.pop() if reports else None,
            "apply_learned": lambda _self, _project: 0,
            "apply_plugin_echoes": lambda _self, _project: 0,
            "refresh": lambda _self, _project: True,
        },
    )()
    studio.plugin_runtime.tick()


def test_learn_on_a_native_parameter(studio: MainWindow) -> None:
    session = studio.session
    master = int(studio.project.mixer_info()["master"])
    assert session.build_and_execute("AddSlot", master, "Delay")
    slot = int(studio.project.mixer_info()["inserts"][0]["slots"][-1]["id"])
    editor = generic_for_slot(session, master, slot, "Delay")
    name = next(iter(editor.descriptors))
    action = learn_action(editor, name)
    assert action.isEnabled()
    engine = studio.plugin_runtime.engine
    try:
        action.trigger()
        assert studio.plugin_runtime.armed_path == f"insert.{master}.slot.{slot}.{name}"
        bind_next_control(
            studio,
            {
                "kind": "cc",
                "number": 74,
                "channel": 1,
                "device": "",
                "value": 0.5,
                "pressed": False,
            },
        )
    finally:
        studio.plugin_runtime.engine = engine
    bindings = studio.project.midi_bindings()
    assert [(b["kind"], b["number"], b["path"]) for b in bindings] == [
        ("cc", 74, f"insert.{master}.slot.{slot}.{name}")
    ]


def test_learn_on_a_plugin_parameter(studio: MainWindow) -> None:
    presets = ("windows-x64-release", "windows-x64-relwithdebinfo", "windows-x64-debug")
    built = [
        p
        for preset in presets
        for p in sorted((ROOT / "build" / preset / "fixtures" / "plugins").glob("*.clap"))
    ]
    if not built or not Plugins.available():
        pytest.skip("adx_test_plugin.clap or the plugin host not built")
    session = studio.session
    master = int(studio.project.mixer_info()["master"])
    path = str(built[0])
    assert session.build_and_execute(
        "AddPluginSlot", master, "clap", "com.adx.test.effect", path, "adX Test Effect"
    )
    slot = int(studio.project.mixer_info()["inserts"][0]["slots"][-1]["id"])
    editor = generic_for_slot(session, master, slot, "clap")
    assert set(editor.descriptors) == {"gain", "mode", "latency", "seed"}
    action = learn_action(editor, "gain")
    assert action.isEnabled()
    engine = studio.plugin_runtime.engine
    try:
        action.trigger()
        # The parameter was not in the slot: learn put it there first, so it has a path.
        assert studio.project.param_valid(f"insert.{master}.slot.{slot}.gain")
        bind_next_control(
            studio,
            {"kind": "cc", "number": 7, "channel": 0, "device": "", "value": 1.0, "pressed": False},
        )
    finally:
        studio.plugin_runtime.engine = engine
    assert studio.project.midi_bindings()[-1]["path"] == f"insert.{master}.slot.{slot}.gain"
