"""MIDI learn and routes from Python (phase_9.md 3 step 2): the project API, and learn
mode's arm-then-bind through the runtime, with an engine that reports a control."""

from __future__ import annotations

import pathlib
from collections.abc import Callable
from typing import Any

import pytest

from adx.engine_bridge import Project
from adx.plugin_runtime import PluginRuntime, control_text, describe_notice

TEXT = (
    "[PROJECT]\nADX_VERSION=2\n"
    "\n[CHANNEL Lead]\nINSTRUMENT=testtone\n"
    "\n[CHANNEL Pad]\nINSTRUMENT=testtone\n"
    '\n[MIXER]\nINSERT 1 name="Master"\n'
)


def test_bindings_add_list_remove_and_save(tmp_path: pathlib.Path) -> None:
    project, _ = Project.loads(TEXT)
    project.add_midi_binding("cc", 74, "channel.Lead.pan", channel=2, maximum=0.5)
    project.add_midi_binding("note", 36, "channel.Pad.volume", toggle=True)
    rows = project.midi_bindings()
    assert [(r["kind"], r["number"], r["path"]) for r in rows] == [
        ("cc", 74, "channel.Lead.pan"),
        ("note", 36, "channel.Pad.volume"),
    ]
    assert rows[0]["channel"] == 2
    assert rows[1]["toggle"]
    # Learning the same control again moves it.
    project.add_midi_binding("cc", 74, "channel.Pad.pan", channel=2)
    assert [r["path"] for r in project.midi_bindings()] == ["channel.Pad.volume", "channel.Pad.pan"]
    path = tmp_path / "learned.adx"
    project.save(str(path))
    text = path.read_text(encoding="utf-8")
    assert (
        "[MIDI]\nLEARN note 36 channel.Pad.volume toggle=yes\n"
        "LEARN cc 74 channel.Pad.pan channel=2\n" in text
    )
    again, _ = Project.load(str(path))
    assert again.midi_bindings() == project.midi_bindings()
    again.remove_midi_binding(0)
    assert len(again.midi_bindings()) == 1
    assert again.undo()
    assert len(again.midi_bindings()) == 2
    with pytest.raises(ValueError, match="no such parameter"):
        project.add_midi_binding("cc", 1, "channel.Nobody.pan")


def test_routes_round_trip() -> None:
    project, _ = Project.loads(TEXT)
    lead = next(c["id"] for c in project.channels_info() if c["name"] == "Lead")
    project.set_midi_routes([{"channel": lead, "key_low": 0, "key_high": 59, "transpose": 12}])
    routes = project.midi_routes()
    assert routes[0]["channel"] == lead
    assert (routes[0]["key_high"], routes[0]["transpose"]) == (59, 12)
    project.set_midi_routes([])
    assert project.midi_routes() == []


class FakeEngine:
    """Reports one control, once, after learn is armed."""

    def __init__(self) -> None:
        self.controls: list[dict[str, Any] | None] = []

    def midi_last_control(self) -> dict[str, Any] | None:
        return self.controls.pop(0) if self.controls else None

    def apply_learned(self, _project: Project) -> int:
        return 0

    def apply_plugin_echoes(self, _project: Project) -> int:
        return 0

    def refresh(self, _project: Project) -> bool:
        return True


class FakeSession:
    def __init__(self, project: Project) -> None:
        self.project = project
        self.notified = 0

    def call(self, method: Callable[..., object], *args: object) -> object:
        return method(*args)

    def notify(self) -> None:
        self.notified += 1


def test_learn_arms_then_binds_the_next_control(qapp: object) -> None:
    project, _ = Project.loads(TEXT)
    engine = FakeEngine()
    runtime = PluginRuntime(engine, FakeSession(project), start=False)  # type: ignore[arg-type]
    learned: list[tuple[str, str]] = []
    runtime.learned.connect(lambda path, text: learned.append((path, text)))
    runtime.arm("channel.Lead.pan")
    assert runtime.armed_path == "channel.Lead.pan"
    # A key's release is not a choice; the next press-or-turn is.
    engine.controls = [
        {"kind": "note", "number": 60, "channel": 1, "device": "", "value": 0.0, "pressed": False},
    ]
    runtime.tick()
    assert runtime.armed_path == "channel.Lead.pan"
    engine.controls = [
        {"kind": "cc", "number": 21, "channel": 3, "device": "Pads", "value": 0.4, "pressed": False}
    ]
    runtime.tick()
    assert runtime.armed_path == ""
    assert learned == [("channel.Lead.pan", "CC 21 on channel 3")]
    binding = project.midi_bindings()[0]
    assert (binding["kind"], binding["number"], binding["channel"], binding["device"]) == (
        "cc",
        21,
        3,
        "Pads",
    )


def test_texts() -> None:
    assert control_text({"kind": "bend", "number": 0, "channel": 0}) == "pitch bend"
    assert control_text({"kind": "note", "number": 36, "channel": 10}) == "note 36 on channel 10"
    assert describe_notice(
        {"kind": "crashed", "plugin": "Pro-Q", "message": "the plugin crashed", "instance": 3}
    ).startswith("Pro-Q crashed")
